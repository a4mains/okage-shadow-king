#include "PS2VM.h"
#include "PS2VM_Preferences.h"
#include "AppConfig.h"
#include "gs/GSH_Null.h"
#include "gs/GsPixelFormats.h"
#include "iop/IopBios.h"
#include "iop/ioman/OpticalMediaDevice.h"
#include "reference_input.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

fs::path CAppConfig::GetBasePath() const {
    auto path=std::filesystem::current_path()/"local/play-boot-config";
    std::filesystem::create_directories(path); return path.string();
}

namespace {
constexpr const char* pinnedElf="2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5";
constexpr uint32 stopPc=0x100618,entryPc=0x100008,mainPc=0x1002f8;

std::string hex(const unsigned char* d,size_t n) {
    std::ostringstream v;for(size_t i=0;i<n;++i)v<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(d[i]);return v.str();
}
std::string sha(const uint8* p,size_t n) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256(p,static_cast<CC_LONG>(n),digest);return hex(digest,sizeof digest);
}
std::string hx(uint64 v){std::ostringstream o;o<<"\"0x"<<std::hex<<v<<'"';return o.str();}
std::string esc(const std::string& s){
    std::string o;for(unsigned char c:s){
        if(c=='"'||c=='\\'){o+='\\';o+=char(c);}else if(c<0x20||c>=0x7f){char b[8];std::snprintf(b,sizeof b,"\\u%04x",c);o+=b;}else o+=char(c);}
    return o;
}
void writeFresh(const std::filesystem::path& path,const void* data,size_t size) {
    if(std::filesystem::exists(path)||std::filesystem::is_symlink(path))throw std::runtime_error("Refusing existing output "+path.string());
    std::ofstream out(path,std::ios::binary);
    if(!out.write(static_cast<const char*>(data),std::streamsize(size)))throw std::runtime_error("Cannot write "+path.string());
}

class LedgerGS final : public CGSH_Null {
public:
    struct Transfer {
        uint64 seq=0,bitbltbuf=0,trxpos=0,trxreg=0,trxdir=0;uint32 total=0,received=0;
        CC_SHA256_CTX ctx;bool open=false;
        uint64 chunks=0,ct24Unaligned=0,nibbles=0,eeEvent=0,gsOp=0,flip=0;
    };
    // Per-nibble last-writer codes (title mode). 0 = never written since the fill.
    enum WriterCode:uint8{W_NONE=0,W_UPLOAD=1,W_DRAW=2,W_LOCAL=3};
    struct DrawClass{uint64 count=0,firstOp=0,lastOp=0,firstFlip=0,lastFlip=0,firstEe=0,lastEe=0;
                     int x0=4096,y0=4096,x1=-1,y1=-1;bool frame=false,z=false;uint64 nibblesMarked=0;};
    std::mutex mutex;
    std::vector<std::string> lines;
    uint64 regWrites=0,kicks=0,transfers=0,resets=0,localToHost=0,localToLocal=0,clutTransfers=0,hostToLocalDone=0;
    std::map<uint8,uint64> regHistogram;
    Transfer current;
    bool armed=false; // true once the initial fill was applied
    bool title=false;
    std::atomic<uint64>* eeEventCounter=nullptr;
    std::vector<uint8> writer,uploadEver;
    uint64 gsOps=0,flips=0,primitives=0,primitivesSkippedNoWrite=0,unsupportedTargets=0,dedupSkips=0,
           xyz3=0,invalidPrimKicks=0,ct24UnalignedChunks=0;
    std::map<std::array<uint64,10>,DrawClass> drawClasses;
    std::map<std::array<uint64,5>,uint64> clutClasses;
    uint64 drawDetailLines=0,clutDetailLines=0;
    // Called on the GS thread only (or after a full sync).
    void flushTransfer() {
        if(!current.open)return;
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256_Final(digest,&current.ctx);
        std::ostringstream o;
        o<<"{\"kind\":\"gs_transfer\",\"seq\":"<<current.seq<<",\"bitbltbuf\":"<<hx(current.bitbltbuf)
         <<",\"dbp\":"<<(current.bitbltbuf>>32&0x3fff)<<",\"dbw\":"<<(current.bitbltbuf>>48&0x3f)
         <<",\"dpsm\":"<<(current.bitbltbuf>>56&0x3f)<<",\"trxpos\":"<<hx(current.trxpos)
         <<",\"dsax\":"<<(current.trxpos>>32&0x7ff)<<",\"dsay\":"<<(current.trxpos>>48&0x7ff)
         <<",\"trxreg\":"<<hx(current.trxreg)<<",\"rrw\":"<<(current.trxreg&0xfff)<<",\"rrh\":"<<(current.trxreg>>32&0xfff)
         <<",\"trxdir\":"<<(current.trxdir&3)<<",\"expected_bytes\":"<<current.total<<",\"received_bytes\":"<<current.received
         <<",\"complete\":"<<(current.received==current.total?"true":"false")
         <<",\"image_sha256\":\""<<hex(digest,sizeof digest)<<"\"";
        if(title)o<<",\"gs_op\":"<<current.gsOp<<",\"flip\":"<<current.flip<<",\"ee_event\":"<<current.eeEvent
                   <<",\"chunks\":"<<current.chunks<<",\"ct24_chunks_not_multiple_of_3\":"<<current.ct24Unaligned
                   <<",\"nibbles_marked\":"<<current.nibbles;
        o<<"}";
        lines.push_back(o.str());current.open=false;
    }
private:
    // ---- Title nibble marking -------------------------------------------------------------
    std::set<std::array<uint32,9>> drawDedup;
    uint64 vq[3]={0,0,0};unsigned vqCount=0;
    uint64 pendingTex0=0;
    uint64 eeNow()const{return eeEventCounter?eeEventCounter->load():0;}
    void markNibble(uint32 nib,uint8 code){
        nib&=(CGSHandler::RAMSIZE*2-1);
        uint8& b=writer[nib>>1];
        if(nib&1)b=uint8((b&0x0f)|(code<<4));else b=uint8((b&0xf0)|code);
        if(code==W_UPLOAD)uploadEver[nib>>1]|=uint8(1u<<(nib&1));
    }
    // nibble mask bit k = nibble k of the unit at byte offset `byteAddr` (unit of 1, 2 or 4 bytes)
    uint64 markUnit(uint32 byteAddr,uint32 nibMask,uint8 code){
        uint64 n=0;for(uint32 k=0;k<8;++k)if(nibMask>>k&1){markNibble(byteAddr*2+k,code);++n;}return n;
    }
    template<typename Storage>
    uint32 pixelByte(uint32 base,uint32 bw,uint32 x,uint32 y){
        CGsPixelFormats::CPixelIndexor<Storage> ix(writer.data(),base,bw);
        return uint32(reinterpret_cast<uint8*>(ix.GetPixelAddress(x,y))-writer.data());
    }
    uint32 t4Nibble(uint32 base,uint32 bw,uint32 x,uint32 y){
        const uint32* offs=CGsPixelFormats::CPixelIndexorPSMT4::GetPageOffsets(); // nibble offsets in a page [128][128]
        const uint32 page=(x/128)+(y/128)*(bw*64)/128;
        return ((base+page*CGsPixelFormats::PAGESIZE)*2+offs[(y%128)*128+(x%128)])&(CGSHandler::RAMSIZE*2-1);
    }
    // Mark one pixel of storage format psm at (x,y): returns marked nibble count, or ~0 if unsupported.
    uint64 markPixel(uint32 psm,uint32 base,uint32 bw,uint32 x,uint32 y,uint32 mask32,uint8 code){
        using F=CGsPixelFormats;
        switch(psm){
        case PSMCT32:case PSMZ32:  return markUnit(psm==PSMCT32?pixelByte<F::STORAGEPSMCT32>(base,bw,x,y):pixelByte<F::STORAGEPSMZ32>(base,bw,x,y),mask32&0xff,code);
        case PSMCT24:case PSMZ24:  return markUnit(psm==PSMCT24?pixelByte<F::STORAGEPSMCT32>(base,bw,x,y):pixelByte<F::STORAGEPSMZ32>(base,bw,x,y),mask32&0x3f,code);
        case PSMCT16:  return markUnit(pixelByte<F::STORAGEPSMCT16>(base,bw,x,y),mask32&0xf,code);
        case PSMCT16S: return markUnit(pixelByte<F::STORAGEPSMCT16S>(base,bw,x,y),mask32&0xf,code);
        case PSMZ16:   return markUnit(pixelByte<F::STORAGEPSMZ16>(base,bw,x,y),mask32&0xf,code);
        case PSMZ16S:  return markUnit(pixelByte<F::STORAGEPSMZ16S>(base,bw,x,y),mask32&0xf,code);
        case PSMT8:    return markUnit(pixelByte<F::STORAGEPSMT8>(base,bw,x,y),0x3,code);
        case PSMT8H:   return markUnit(pixelByte<F::STORAGEPSMCT32>(base,bw,x,y),0xc0,code);
        case PSMT4HL:  return markUnit(pixelByte<F::STORAGEPSMCT32>(base,bw,x,y),0x40,code);
        case PSMT4HH:  return markUnit(pixelByte<F::STORAGEPSMCT32>(base,bw,x,y),0x80,code);
        case PSMT4:    markNibble(t4Nibble(base,bw,x,y),code);return 1;
        default: return ~uint64(0);
        }
    }
    // Rectangle [x0,x1]x[y0,y1] (inclusive) of a draw target. Returns marked nibbles.
    uint64 markRect(uint32 psm,uint32 base,uint32 bw,int x0,int y0,int x1,int y1,uint32 mask32){
        std::array<uint32,9> key{W_DRAW,psm,base,bw,uint32(x0),uint32(y0),uint32(x1),uint32(y1),mask32};
        if(!drawDedup.insert(key).second){++dedupSkips;return 0;}
        uint64 n=0;
        for(int y=y0;y<=y1;++y)for(int x=x0;x<=x1;++x){
            const uint64 m=markPixel(psm,base,bw,uint32(x),uint32(y),mask32,W_DRAW);
            if(m==~uint64(0)){++unsupportedTargets;return n;}
            n+=m;
        }
        return n;
    }
    // Nibble mask of a frame-buffer pixel actually writable under FBMSK (conservative for 16-bit formats).
    static uint32 frameNibbles(uint32 psm,uint32 fbmsk,bool rgbOnly){
        uint32 m=0;
        if(psm==PSMCT16||psm==PSMCT16S||psm==PSMZ16||psm==PSMZ16S)return fbmsk==0xffffffffu?0:0xf;
        const unsigned n=(psm==PSMCT24||psm==PSMZ24||rgbOnly)?6:8;
        for(unsigned k=0;k<n;++k)if(((fbmsk>>(4*k))&0xf)!=0xf)m|=1u<<k;
        return m;
    }
    void drawPrimitive(const uint64* v,unsigned nv,uint32 type){
        ++primitives;
        const uint64 prmodecont=m_nReg[GS_REG_PRMODECONT];
        const uint64 prim=m_nReg[GS_REG_PRIM];
        const unsigned ctxt=unsigned(((prmodecont&1)?prim:m_nReg[GS_REG_PRMODE])>>9&1);
        const uint64 frame=m_nReg[GS_REG_FRAME_1+ctxt],zbuf=m_nReg[GS_REG_ZBUF_1+ctxt],test=m_nReg[GS_REG_TEST_1+ctxt];
        const uint64 scissor=m_nReg[GS_REG_SCISSOR_1+ctxt],xyoff=m_nReg[GS_REG_XYOFFSET_1+ctxt];
        const bool tme=((prmodecont&1)?prim:m_nReg[GS_REG_PRMODE])>>4&1;
        const uint64 tex0=tme?m_nReg[GS_REG_TEX0_1+ctxt]:0;
        // Write enables (exact "nothing written" cases; everything else conservatively both).
        bool fw=true,zw=!((zbuf>>32)&1),rgbOnly=false;
        const bool ate=test&1;const uint32 atst=uint32(test>>1&7),afail=uint32(test>>12&3);
        const bool zte=test>>16&1;const uint32 ztst=uint32(test>>17&3);
        if(zte&&ztst==0){fw=false;zw=false;}
        if(ate&&atst==0){
            if(afail==0){fw=false;zw=false;}else if(afail==1)zw=false;else if(afail==2)fw=false;else{zw=false;rgbOnly=true;}
        }
        // Bounding box in window pixels (12.4 fixed point minus XYOFFSET), clipped to SCISSOR.
        const int ofx=int(xyoff&0xffff),ofy=int(xyoff>>32&0xffff);
        int minx=1<<30,miny=1<<30,maxx=-(1<<30),maxy=-(1<<30);
        for(unsigned i=0;i<nv;++i){
            const int x=int(v[i]&0xffff)-ofx,y=int(v[i]>>16&0xffff)-ofy;
            minx=std::min(minx,x);maxx=std::max(maxx,x);miny=std::min(miny,y);maxy=std::max(maxy,y);
        }
        auto floorDiv=[](int a){return a>=0?a/16:-((-a+15)/16);};
        auto ceilDiv=[](int a){return a>=0?(a+15)/16:-((-a)/16);};
        int x0=floorDiv(minx),x1=ceilDiv(maxx),y0=floorDiv(miny),y1=ceilDiv(maxy);
        const int sx0=int(scissor&0x7ff),sx1=int(scissor>>16&0x7ff),sy0=int(scissor>>32&0x7ff),sy1=int(scissor>>48&0x7ff);
        x0=std::max(x0,sx0);y0=std::max(y0,sy0);x1=std::min(x1,sx1);y1=std::min(y1,sy1);
        const uint32 fpsm=uint32(frame>>24&0x3f),fbp=uint32(frame&0x1ff)*8192,fbw=uint32(frame>>16&0x3f),fbmsk=uint32(frame>>32);
        const uint32 zpsm=uint32(0x30|(zbuf>>24&0xf)),zbp=uint32(zbuf&0x1ff)*8192;
        const uint32 fmask=fw?frameNibbles(fpsm,fbmsk,rgbOnly):0;
        if(!fmask)fw=false;
        uint64 marked=0;
        const bool empty=x0>x1||y0>y1;
        if(!empty){
            if(fw)marked+=markRect(fpsm,fbp,fbw,x0,y0,x1,y1,fmask);
            if(zw)marked+=markRect(zpsm,zbp,fbw,x0,y0,x1,y1,0xff);
        }
        if(empty||(!fw&&!zw))++primitivesSkippedNoWrite;
        std::array<uint64,10> key{ctxt,type,uint64(tme),frame,zbuf,test,tex0,scissor,xyoff,uint64(fw)|uint64(zw)<<1};
        auto& c=drawClasses[key];
        if(!c.count){c.firstOp=gsOps;c.firstFlip=flips;c.firstEe=eeNow();}
        ++c.count;c.lastOp=gsOps;c.lastFlip=flips;c.lastEe=eeNow();c.frame|=fw;c.z|=zw;c.nibblesMarked+=marked;
        if(!empty){c.x0=std::min(c.x0,x0);c.y0=std::min(c.y0,y0);c.x1=std::max(c.x1,x1);c.y1=std::max(c.y1,y1);}
        if(drawDetailLines<256){
            ++drawDetailLines;std::ostringstream o;
            o<<"{\"kind\":\"gs_draw\",\"gs_op\":"<<gsOps<<",\"flip\":"<<flips<<",\"ee_event\":"<<eeNow()<<",\"prim_type\":"<<type
             <<",\"ctxt\":"<<ctxt<<",\"tme\":"<<(tme?"true":"false")<<",\"frame\":"<<hx(frame)<<",\"zbuf\":"<<hx(zbuf)<<",\"test\":"<<hx(test)
             <<",\"tex0\":"<<hx(tex0)<<",\"scissor\":"<<hx(scissor)<<",\"xyoffset\":"<<hx(xyoff)<<",\"rect\":"
             <<(empty?std::string("null"):"["+std::to_string(x0)+","+std::to_string(y0)+","+std::to_string(x1)+","+std::to_string(y1)+"]")
             <<",\"writes_frame\":"<<(fw?"true":"false")<<",\"writes_z\":"<<(zw?"true":"false")<<",\"nibbles_marked\":"<<marked<<"}";
            lines.push_back(o.str());
        }
    }
    void vertex(uint64 value,bool kick){
        const uint32 type=uint32(m_nReg[GS_REG_PRIM]&7);
        static const unsigned need[8]={1,2,2,3,3,3,2,0};
        if(type==7){if(kick)++invalidPrimKicks;return;}
        if(vqCount<3)vq[vqCount++]=value;else{vq[0]=vq[1];vq[1]=vq[2];vq[2]=value;}
        if(vqCount<need[type])return;
        if(kick){
            if(type==5){const uint64 fan[3]={vq[0],vq[vqCount-2],vq[vqCount-1]};drawPrimitive(fan,3,type);}
            else drawPrimitive(vq+(vqCount-need[type]),need[type],type);
        }
        switch(type){
        case 0:case 1:case 3:case 6: vqCount=0;break;
        case 2: vq[0]=vq[vqCount-1];vqCount=1;break;
        case 4: {const uint64 a=vq[vqCount-2],b=vq[vqCount-1];vq[0]=a;vq[1]=b;vqCount=2;break;}
        case 5: {const uint64 a=vq[0],b=vq[vqCount-1];vq[0]=a;vq[1]=b;vqCount=2;break;}
        }
    }
protected:
    void WriteRegisterImpl(uint8 reg,uint64 value) override {
        {
            std::lock_guard lock(mutex);
            ++regWrites;++regHistogram[reg&0x7f];
            if(reg==GS_REG_XYZ2||reg==GS_REG_XYZF2) {
                ++kicks;
                if(kicks<=64) {
                    std::ostringstream o;
                    o<<"{\"kind\":\"gs_vertex_kick\",\"n\":"<<kicks<<",\"reg\":"<<unsigned(reg)<<",\"value\":"<<hx(value)
                     <<",\"prim\":"<<hx(m_nReg[GS_REG_PRIM])<<",\"frame1\":"<<hx(m_nReg[GS_REG_FRAME_1])
                     <<",\"zbuf1\":"<<hx(m_nReg[GS_REG_ZBUF_1])<<",\"test1\":"<<hx(m_nReg[GS_REG_TEST_1])<<"}";
                    lines.push_back(o.str());
                }
            }
            if(title) {
                const uint8 r=reg&0x7f;++gsOps;
                if(r==GS_REG_PRIM)vqCount=0;
                else if(r==GS_REG_XYZ2||r==GS_REG_XYZF2)vertex(value,true);
                else if(r==GS_REG_XYZ3||r==GS_REG_XYZF3){++xyz3;vertex(value,false);}
                else if(r==GS_REG_TEX0_1||r==GS_REG_TEX0_2)pendingTex0=value;
                else if(r==GS_REG_TEX2_1||r==GS_REG_TEX2_2){
                    const uint64 mask=0xFFFFFFE003F00000ULL;
                    pendingTex0=(m_nReg[GS_REG_TEX0_1+(r-GS_REG_TEX2_1)]&~mask)|(value&mask);
                }
            }
        }
        CGSH_Null::WriteRegisterImpl(reg,value);
    }
    void BeginTransferWrite() override {
        std::lock_guard lock(mutex);
        flushTransfer();
        current=Transfer{};current.open=true;current.seq=++transfers;
        current.bitbltbuf=m_nReg[GS_REG_BITBLTBUF];current.trxpos=m_nReg[GS_REG_TRXPOS];
        current.trxreg=m_nReg[GS_REG_TRXREG];current.trxdir=m_nReg[GS_REG_TRXDIR];
        current.total=m_trxCtx.nSize;CC_SHA256_Init(&current.ctx);
        if(title){current.gsOp=gsOps;current.flip=flips;current.eeEvent=eeNow();}
        CGSH_Null::BeginTransferWrite();
    }
    void TransferWrite(const uint8* data,uint32 length) override {
        {
            std::lock_guard lock(mutex);
            if(current.open){
                const uint32 take=std::min<uint32>(length,m_trxCtx.nSize);
                CC_SHA256_Update(&current.ctx,data,take);current.received+=take;
            }
            if(title) {
                ++current.chunks;drawDedup.clear();
                const uint32 dpsm=uint32(m_nReg[GS_REG_BITBLTBUF]>>56&0x3f);
                const uint32 dbp=uint32(m_nReg[GS_REG_BITBLTBUF]>>32&0x3fff)*256,dbw=uint32(m_nReg[GS_REG_BITBLTBUF]>>48&0x3f);
                const uint32 dsax=uint32(m_nReg[GS_REG_TRXPOS]>>32&0x7ff),dsay=uint32(m_nReg[GS_REG_TRXPOS]>>48&0x7ff);
                const uint32 rrw=uint32(m_nReg[GS_REG_TRXREG]&0xfff);
                const uint32 len=std::min<uint32>(length,m_trxCtx.nSize);
                uint64 pixels=0;
                switch(dpsm){
                case PSMCT32:pixels=len/4;break;
                case PSMCT24:pixels=(len+2)/3;if(len%3){++current.ct24Unaligned;++ct24UnalignedChunks;}break;
                case PSMCT16:case PSMCT16S:pixels=len/2;break;
                case PSMT8:case PSMT8H:pixels=len;break;
                case PSMT4:case PSMT4HL:case PSMT4HH:pixels=uint64(len)*2;break;
                default:++unsupportedTargets;break;
                }
                uint32 rrx=m_trxCtx.nRRX,rry=m_trxCtx.nRRY;
                for(uint64 i=0;i<pixels;++i){
                    const uint32 x=(rrx+dsax)%2048,y=(rry+dsay)%2048;
                    const uint64 m=markPixel(dpsm,dbp,dbw,x,y,0xff,W_UPLOAD);
                    if(m!=~uint64(0))current.nibbles+=m;
                    if(++rrx==rrw){rrx=0;++rry;}
                }
            }
        }
        CGSH_Null::TransferWrite(data,length);
    }
    void ProcessHostToLocalTransfer() override {std::lock_guard lock(mutex);++hostToLocalDone;}
    void ProcessLocalToHostTransfer() override {std::lock_guard lock(mutex);++localToHost;lines.push_back("{\"kind\":\"gs_local_to_host\"}");}
    void ProcessLocalToLocalTransfer() override {
        std::lock_guard lock(mutex);++localToLocal;
        if(!title){lines.push_back("{\"kind\":\"gs_local_to_local\"}");return;}
        // Not executed by the null renderer: the destination is marked as written by an unmodelled writer.
        drawDedup.clear();
        const uint64 bb=m_nReg[GS_REG_BITBLTBUF],tp=m_nReg[GS_REG_TRXPOS],tr=m_nReg[GS_REG_TRXREG];
        const uint32 dpsm=uint32(bb>>56&0x3f),dbp=uint32(bb>>32&0x3fff)*256,dbw=uint32(bb>>48&0x3f);
        const uint32 dsax=uint32(tp>>32&0x7ff),dsay=uint32(tp>>48&0x7ff),rrw=uint32(tr&0xfff),rrh=uint32(tr>>32&0xfff);
        uint64 n=0;bool unsupported=false;
        for(uint32 y=0;y<rrh&&!unsupported;++y)for(uint32 x=0;x<rrw;++x){
            const uint64 m=markPixel(dpsm,dbp,dbw,(dsax+x)%2048,(dsay+y)%2048,0xff,W_LOCAL);
            if(m==~uint64(0)){unsupported=true;++unsupportedTargets;break;}
            n+=m;
        }
        std::ostringstream o;
        o<<"{\"kind\":\"gs_local_to_local\",\"gs_op\":"<<gsOps<<",\"flip\":"<<flips<<",\"ee_event\":"<<eeNow()<<",\"bitbltbuf\":"<<hx(bb)
         <<",\"trxpos\":"<<hx(tp)<<",\"trxreg\":"<<hx(tr)<<",\"dpsm\":"<<dpsm<<",\"dbp\":"<<(dbp/256)<<",\"dbw\":"<<dbw
         <<",\"sbp\":"<<(bb&0x3fff)<<",\"sbw\":"<<(bb>>16&0x3f)<<",\"spsm\":"<<(bb>>24&0x3f)
         <<",\"rrw\":"<<rrw<<",\"rrh\":"<<rrh<<",\"nibbles_marked\":"<<n<<",\"unsupported\":"<<(unsupported?"true":"false")<<"}";
        lines.push_back(o.str());
    }
    void ProcessClutTransfer(uint32 a,uint32 b) override {
        std::lock_guard lock(mutex);++clutTransfers;
        if(!title){lines.push_back("{\"kind\":\"gs_clut_transfer\",\"a\":"+std::to_string(a)+",\"b\":"+std::to_string(b)+"}");return;}
        // CLUT loads read local memory into the CLUT buffer; they do not write local memory.
        const uint64 t=pendingTex0;
        std::array<uint64,5> key{t>>37&0x3fff,t>>51&0xf,t>>55&1,t>>56&0x1f,t>>61&7};
        ++clutClasses[key];
        if(clutDetailLines<64){
            ++clutDetailLines;
            lines.push_back("{\"kind\":\"gs_clut_transfer\",\"gs_op\":"+std::to_string(gsOps)+",\"flip\":"+std::to_string(flips)
                            +",\"a\":"+std::to_string(a)+",\"b\":"+std::to_string(b)+",\"tex0\":"+hx(t)+",\"cbp\":"+std::to_string(key[0])
                            +",\"cpsm\":"+std::to_string(key[1])+",\"csm\":"+std::to_string(key[2])+",\"csa\":"+std::to_string(key[3])
                            +",\"cld\":"+std::to_string(key[4])+"}");
        }
    }
    void MarkNewFrame() override {{std::lock_guard lock(mutex);++flips;}CGSH_Null::MarkNewFrame();}
public:
    // Title-mode summary lines (after a full GS sync).
    void writeTitleSummary(std::ostream& o){
        for(const auto& kv:drawClasses){
            const auto& k=kv.first;const auto& c=kv.second;
            o<<"{\"kind\":\"gs_draw_class\",\"ctxt\":"<<k[0]<<",\"prim_type\":"<<k[1]<<",\"tme\":"<<(k[2]?"true":"false")
             <<",\"frame\":"<<hx(k[3])<<",\"fbp\":"<<(k[3]&0x1ff)<<",\"fbw\":"<<(k[3]>>16&0x3f)<<",\"fpsm\":"<<(k[3]>>24&0x3f)<<",\"fbmsk\":"<<hx(k[3]>>32)
             <<",\"zbuf\":"<<hx(k[4])<<",\"zbp\":"<<(k[4]&0x1ff)<<",\"zpsm\":"<<(0x30|(k[4]>>24&0xf))<<",\"zmsk\":"<<(k[4]>>32&1)
             <<",\"test\":"<<hx(k[5])<<",\"tex0\":"<<hx(k[6])<<",\"tbp0\":"<<(k[6]&0x3fff)<<",\"tbw\":"<<(k[6]>>14&0x3f)<<",\"tpsm\":"<<(k[6]>>20&0x3f)
             <<",\"scissor\":"<<hx(k[7])<<",\"xyoffset\":"<<hx(k[8])<<",\"writes_frame\":"<<(c.frame?"true":"false")<<",\"writes_z\":"<<(c.z?"true":"false")
             <<",\"count\":"<<c.count<<",\"first_gs_op\":"<<c.firstOp<<",\"last_gs_op\":"<<c.lastOp<<",\"first_flip\":"<<c.firstFlip<<",\"last_flip\":"<<c.lastFlip
             <<",\"first_ee_event\":"<<c.firstEe<<",\"last_ee_event\":"<<c.lastEe
             <<",\"rect_union\":"<<(c.x1<0?std::string("null"):"["+std::to_string(c.x0)+","+std::to_string(c.y0)+","+std::to_string(c.x1)+","+std::to_string(c.y1)+"]")
             <<",\"nibbles_marked\":"<<c.nibblesMarked<<"}\n";
        }
        for(const auto& kv:clutClasses)
            o<<"{\"kind\":\"gs_clut_class\",\"cbp\":"<<kv.first[0]<<",\"cpsm\":"<<kv.first[1]<<",\"csm\":"<<kv.first[2]<<",\"csa\":"<<kv.first[3]
             <<",\"cld\":"<<kv.first[4]<<",\"count\":"<<kv.second<<"}\n";
        o<<"{\"kind\":\"gs_title_summary\",\"gs_ops\":"<<gsOps<<",\"flips\":"<<flips<<",\"primitives\":"<<primitives
         <<",\"primitives_without_write\":"<<primitivesSkippedNoWrite<<",\"xyz3\":"<<xyz3<<",\"invalid_prim_kicks\":"<<invalidPrimKicks
         <<",\"draw_rect_dedup_skips\":"<<dedupSkips<<",\"unsupported_targets\":"<<unsupportedTargets
         <<",\"ct24_chunks_not_multiple_of_3\":"<<ct24UnalignedChunks<<",\"draw_classes\":"<<drawClasses.size()<<",\"clut_classes\":"<<clutClasses.size()<<"}\n";
    }
    void ResetImpl() override {
        {std::lock_guard lock(mutex);++resets;lines.push_back("{\"kind\":\"gs_reset\",\"armed\":"+std::string(armed?"true":"false")+"}");}
        CGSH_Null::ResetImpl();
    }
};

class ResearchVM final : public CPS2VM {
public:
    void wait_for_observer_command(){m_mailBox.WaitForCall(100);}
};

class TraceDisc final : public Iop::Ioman::COpticalMediaDevice {
public:
    std::mutex mutex;std::vector<std::string> rows;std::atomic<int>* topStep;
    TraceDisc(OpticalMediaPtr& media,std::atomic<int>* step):COpticalMediaDevice(media),topStep(step) {}
    Framework::CStream* GetFile(uint32 flags,const char* path) override {
        auto* result=COpticalMediaDevice::GetFile(flags,path);
        std::string hash="null";uint64 size=0;
        if(result) {
            std::unique_ptr<Framework::CStream> again(COpticalMediaDevice::GetFile(flags,path));
            if(again) {
                size=again->GetLength();
                if(size<=512ull*1024*1024) {
                    CC_SHA256_CTX ctx;CC_SHA256_Init(&ctx);std::vector<uint8> buf(1<<20);
                    for(uint64 left=size;left;) {
                        const auto n=std::min<uint64>(left,buf.size());
                        if(again->Read(buf.data(),n)!=n)break;
                        CC_SHA256_Update(&ctx,buf.data(),CC_LONG(n));left-=n;
                    }
                    unsigned char digest[CC_SHA256_DIGEST_LENGTH];CC_SHA256_Final(digest,&ctx);hash='"'+hex(digest,sizeof digest)+'"';
                }
            }
        }
        std::lock_guard lock(mutex);
        std::ostringstream o;
        o<<"{\"kind\":\"disc_open\",\"n\":"<<rows.size()<<",\"flags\":"<<flags<<",\"path\":\""<<esc(path)<<"\",\"found\":"
         <<(result?"true":"false")<<",\"size\":"<<size<<",\"sha256\":"<<hash<<",\"top_step_pc\":"<<topStep->load()<<"}";
        rows.push_back(o.str());
        return result;
    }
};

struct HookDef{uint32 addr;const char* name;};
// Requested set first, then supporting functions needed to attribute them.
const HookDef hookDefs[]={
    {0x1cf008,"alloc"},{0x1cee50,"retain"},{0x1cf728,"free"},{0x1c9bb8,"upload"},
    {0x1cc880,"handle_create"},{0x1ccd48,"handle_alias"},{0x1cc4e0,"handle_name"},{0x1cca30,"handle_retire"},
    {0x1cd298,"palette_bind"},{0x1cd7d0,"clut_alloc_upload"},{0x1cd1e0,"image_alloc_handle_upload"},
    {0x1cb1e8,"color_depth_init"},{0x18f8c0,"capture_layer_init"},{0x190f38,"filter_clut_a"},
    {0x1908b0,"display_list_a_record"},
    {0x1cba48,"transfer"},{0x1cd598,"palette_bind_desc"},{0x18f7a8,"xim_load"},{0x1c2e68,"file_read"},
    {0x1c29d8,"file_read_raw"},{0x1bccf0,"archive_mount"},{0x1bcc80,"archive_unmount"},{0x1c0930,"dir_prefix"},
    {0x192ea0,"noise_texture"},{0x1974f8,"glyph_cache"},{0x1947a0,"font_load"},{0x1001a8,"fonts_init"},
    {0x1011d0,"effects_init"},{0x18fc68,"capture_filter_init"},{0x1943a8,"filter_mesh_noise"},
    {0x192e28,"filter_clut_b_setup"},{0x191030,"filter_clut_b"},{0x1914d0,"display_list_b_record"},
    {0x1cad20,"dma_kick"},{0x1ce940,"allocator_init"},{0x1cbdf8,"handle_system_init"},{0x10e098,"effect_noise_grid"},
    {0x1002f8,"main"},{0x103a88,"effects_load"},{0x103718,"effect_textures"},{0x1cce78,"handle_lookup"},
    {0x1cd5c8,"handle_reserve"},{0x186aa8,"xmd_load"},{0x1cebe8,"clear_reservation"},{0x1cdb80,"layer_build"},
    {0x1cdda0,"layer_attach_color"},{0x1cde18,"layer_attach_depth"},
    {0x1cf910,"occupancy_probe"},{0x10bc78,"effect_script_init"},
};
constexpr uint32 randPc=0x1f14b0;
const HookDef rngHookDefs[]={
    {0x10ba38,"efd_import"},{0x10b7a0,"efd_define"},{0x10b5e8,"efd_reference"},{0x10b318,"efd_range"},
    {0x10a970,"template_request"},{0x113508,"template_share"},
    {0x10e138,"factory_noise_shape"},{0x10dbc0,"factory_rays"},{0x111768,"factory_surface"},{0x10e918,"factory_spray"},
    {0x10ec68,"factory_10ec68"},{0x10faa0,"factory_10faa0"},{0x110b28,"factory_110b28"},{0x10fe28,"factory_10fe28"},
    {0x110070,"factory_110070"},{0x1106b8,"factory_1106b8"},{0x110528,"factory_110528"},{0x110908,"factory_110908"},
    {0x1107e8,"factory_1107e8"},{0x10fca8,"factory_10fca8"},{0x10f1b8,"factory_10f1b8"},{0x1111d0,"factory_1111d0"},
    {0x10e4e0,"factory_10e4e0"},{0x110db8,"factory_110db8"},{0x110c88,"factory_110c88"},{0x10e5f8,"factory_10e5f8"},
    {0x110298,"factory_110298"},{0x10f690,"factory_10f690"},{0x10e690,"factory_10e690"},
};
constexpr uint32 cacheDefTable=0x24e790,cacheElemTable=0x24e898,gridPtr=0x24e9fc,templateTablePtr=0x1f849c,currentSlot=0x24ea24;
const HookDef titleHookDefs[]={
    {0x12c350,"mode8_construct"},{0x12bfc8,"mode8_update"},{0x129570,"logo_tick"},{0x1296f8,"title_update"},
    {0x124930,"mode_request"},{0x16c3c8,"storage_init"},{0x16c490,"storage_probe"},
    {0x100a60,"image_list_load"},{0x1009e0,"image_list_append"},{0x1b2348,"image_load_append"},
    {0x101058,"resource_retire"},{0x1b5fd0,"image_slots_retire"},{0x1b2240,"image_list_retire"},
    {0x186c48,"xmd_load_path"},{0x183450,"xmd_bind"},{0x182f08,"xmd_model_build"},{0x182e28,"material_table"},
    {0x1cc778,"handle_successor"},{0x1c2e90,"file_load"},{0x1c9b48,"upload_qwc"},{0x1cbdd8,"reserve_class_set"},
    {0x186a50,"cache_enable_set"},{0x186a40,"lower_enable_set"},{0x1bba88,"cached_model_loader"},
    {0x1cfc00,"range_alloc"},
};
constexpr uint32 imageListBase=0x337508,imageListCount=0x337608,reserveClass=0x3abb14,cacheSwitch=0x1fe310,lowerSwitch=0x1fe30c,
                 materialListHead=0x330144,storageState=0x2597a8;
const HookDef titleRetireHookDefs[]={
    {0x1c0160,"pad_pressed"},{0x1c01d0,"pad_repeat"},{0x1c0330,"pad_analog"},
    {0x101960,"effect_alloc"},{0x1294b0,"effect_create_title"},{0x10c188,"effect_bind"},{0x10bec0,"effect_rebind"},
    {0x103640,"effect_remove_request"},{0x101648,"effect_remove_checked"},{0x102218,"effect_service"},
    {0x103bf8,"effect_free"},
    {0x124548,"successor_construct"},{0x124818,"mode_parent_tick"},{0x1246b0,"mode_child_create"},{0x1151b8,"node_create"},
    {0x12c8d8,"mode8_destroy"},{0x12dd50,"theme_retire_request"},{0x1bdaf0,"host_free"},
    {0x179650,"model_table_retire"},{0x1797b8,"model_group_retire"},{0x1b84b0,"scene_retire"},{0x1cfca8,"range_free"},
    {0x1b5fc8,"resource_slot_retire"},{0x12df80,"title_sound"},{0x124f10,"reset_game"},{0x1251d8,"reset_progress"},
    {0x12db50,"theme_start"},
    // Open questions O01..O12 of the title-retirement input contract (static source contract).
    {0x1cede8,"desc_refs_query"},{0x10c7b8,"effect_template_bind"},{0x1b8a80,"title_instance_create"},
    {0x1b6f08,"instance_retire"},{0x1b6400,"instance_release"},{0x1b4c18,"image_slot_release"},
    {0x1b4b00,"image_list_release_tail"},{0x1976f0,"glyph_cache_transfer"},
};
const HookDef titleRetireProbeDefs[]={
    {0x1024d0,"effect_service_callback_return"},{0x102e4c,"effect_render_callback_return"},
    {0x103ba8,"effect_heap_alloc_return"},{0x103c28,"effect_heap_free_return"},{0x103c70,"effect_heap_realloc_return"},
};
constexpr uint32 effectTablePtr=0x1f849c,effectSlots=0x54,padMaskTable=0x1f85c8,dirFlags=0x259964,dirRepeat=0x259960;
// PS2::CControllerInfo::BUTTON indices used by the fixed Retire/Restore pad scripts.
constexpr uint32 btnLeft=6,btnStart=9,btnCircle=12,btnCross=13;
const HookDef restoreHookDefs[]={
    {0x100ef8,"resource_construct"},{0x100f90,"resource_tick"},{0x1980f8,"ui_pass"},
    {0x1243c8,"restore_resources_load"},{0x11fb78,"atlas_load"},{0x1a9190,"ruw_load"},{0x11b2f0,"restore_select"},
    {0x12d470,"audio_12d470"},
    {0x11f5f8,"manager_create"},{0x11f668,"manager_construct"},{0x11f778,"window_construct"},{0x11b518,"restore_window_init"},
    {0x11fec8,"manager_input_pass"},{0x120ad0,"manager_update_pass"},{0x11e480,"restore_input"},{0x11c2c8,"restore_update"},
    {0x11faf8,"window_fade_out"},{0x11faa8,"window_fade_in"},
    {0x120b48,"manager_destroy"},{0x199458,"window_destroy"},
    {0x124628,"wrapper_destroy"},{0x124448,"wrapper_cleanup"},{0x1a9888,"ui_release_1a9888"},{0x199e88,"ui_release_199e88"},
    {0x19c568,"ui_place_19c568"},{0x19ca20,"ui_place_19ca20"},
    {0x1958e0,"text_queue_1958e0"},{0x195ad0,"text_queue_195ad0"},{0x1951e8,"font_get"},
    {0x16d580,"mc_check_16d580"},{0x16d708,"mc_16d708"},{0x16d748,"mc_16d748"},{0x16d788,"mc_16d788"},{0x16d7f8,"mc_16d7f8"},
    {0x16cdb8,"mc_read_16cdb8"},{0x16c2a0,"mc_sync_16c2a0"},{0x16c370,"mc_16c370"},
    {0x12ce20,"theme_tick"},{0x12de98,"theme_end_12de98"},{0x12dcb0,"theme_release_12dcb0"},
    {0x124630,"wrapper_tick"},{0x1998a8,"window_opacity_step"},{0x1a4e50,"opacity_get"},{0x1a99e0,"window_mode_set"},
    {0x16d4f8,"mc_job_wait_16d4f8"},{0x1248d8,"mode_result_set"},{0x1cc2b0,"handle_desc_get"},
    {0x197c68,"glyph_cache_lookup"},{0x195ff0,"glyph_path_195ff0"},{0x198390,"ui_layout"},{0x19d0c8,"ui_layout_19d0c8"},
    {0x19eec8,"ui_layout_mode0"},{0x1a1ad8,"ui_layout_mode2"},{0x175398,"fn_175398"},
};
const HookDef restoreProbeDefs[]={
    {0x1984b0,"ui_layout_rate_check"},{0x199fdc,"ui_release_199ed4_taken"},{0x199edc,"ui_release_199ed4_not_taken"},
    {0x1aee04,"packet_hook_return_1aedfc"},{0x1af28c,"packet_hook_return_1af284"},{0x1af6f4,"packet_hook_return_1af6ec"},
    {0x1afa18,"packet_hook_return_1afa10"},
};
constexpr uint32 restoreState=0x1f89a8,restoreSel=0x1f89ac,restoreKind=0x1f89a0,restoreAtlasGlobal=0x24f730,cancelMaskAddr=0x1f85e0;

class ScriptedPad final : public CPadHandler {
public:
    std::atomic<uint32> buttons{0};
    std::atomic<unsigned>* vblanks=nullptr;
    std::mutex mutex;std::vector<std::string> rows;
    void Update(uint8* ram) override {
        ++frame;const uint32 b=buttons.load();
        for(auto* listener:m_interfaces)for(unsigned i=0;i<PS2::CControllerInfo::MAX_BUTTONS;++i){
            const auto button=static_cast<PS2::CControllerInfo::BUTTON>(i);
            if(PS2::CControllerInfo::IsAxis(button))listener->SetAxisState(0,button,128,ram);
            else listener->SetButtonState(0,button,(b>>i)&1,ram);
        }
        if(frame==1||b!=last){
            unsigned mask=0;
            for(unsigned i=4;i<PS2::CControllerInfo::MAX_BUTTONS;++i)if(b>>i&1)mask|=CPadInterface::GetButtonMask(static_cast<PS2::CControllerInfo::BUTTON>(i));
            std::lock_guard lock(mutex);
            rows.push_back("{\"kind\":\"pad_update\",\"update\":"+std::to_string(frame)+",\"vb\":"+std::to_string(vblanks?vblanks->load():0)
                           +",\"buttons\":"+std::to_string(b)+",\"play_mask\":"+std::to_string(mask)+"}");
            last=b;
        }
    }
private:
    unsigned frame=0;uint32 last=~0u;
};

struct Frame{uint32 fn;const char* name;uint32 ra;uint64 sp;uint64 enterIndex;bool dynamicBp;
             std::string pendingName;uint64 a[8];uint32 fileBuf=0;int32 aux=0;uint32 aux2=0;};

struct Alloc{int cls,psm,w,h;uint64 desc;uint64 serial=0;};

int bitsPerPixel(uint32 psm){switch(psm){case 0:return 32;case 19:return 8;case 20:case 36:case 44:return 4;default:return 0;}}
// Title mode also hashes PSMCT24/PSMCT16/PSMCT16S sources (Startup/RNG output unchanged without --title).
int bitsPerPixelTitle(uint32 psm){switch(psm){case 1:return 24;case 2:case 10:return 16;default:return bitsPerPixel(psm);}}
} // namespace

int main(int argc,char** argv) {
    try {
        if(argc<5)throw std::runtime_error("Usage: okage-startup-oracle DISC.cue ELF OUTDIR FILL(00|ff) [--deadline-seconds N] [--minimal | --rng-products | --title [--minimal] | --title-retire restore|newgame [--minimal] | --restore-cancel --memcards DIR [--minimal]]");
        const auto disc=std::filesystem::absolute(argv[1]);
        const std::filesystem::path elfPath=argv[2],outDir=argv[3];
        const std::string fillText=argv[4];
        if(fillText!="00"&&fillText!="ff")throw std::runtime_error("FILL must be 00 or ff");
        const uint8 fill=fillText=="ff"?0xff:0x00;
        unsigned deadlineSeconds=1800;bool minimal=false,rngProducts=false,title=false,retire=false,restoreCancel=false;std::string route;
        std::filesystem::path memcards;
        for(int i=5;i<argc;++i){
            if(std::string(argv[i])=="--deadline-seconds"&&i+1<argc)deadlineSeconds=unsigned(std::stoul(argv[++i]));
            else if(std::string(argv[i])=="--minimal")minimal=true;
            else if(std::string(argv[i])=="--rng-products")rngProducts=true;
            else if(std::string(argv[i])=="--title")title=true;
            else if(std::string(argv[i])=="--title-retire"&&i+1<argc){retire=true;route=argv[++i];}
            else if(std::string(argv[i])=="--restore-cancel")restoreCancel=true;
            else if(std::string(argv[i])=="--memcards"&&i+1<argc)memcards=argv[++i];
            else throw std::runtime_error("Unknown argument");
        }
        if(minimal&&rngProducts)throw std::runtime_error("--minimal and --rng-products are exclusive");
        if(title&&rngProducts)throw std::runtime_error("--title already includes the --rng-products observations");
        if(!memcards.empty()&&!restoreCancel)throw std::runtime_error("--memcards is only used with --restore-cancel");
        if(restoreCancel){
            if(retire||title||rngProducts)throw std::runtime_error("--restore-cancel is exclusive with --title/--title-retire/--rng-products");
            if(memcards.empty())throw std::runtime_error("--restore-cancel needs --memcards DIR (isolated, empty mc0/ and mc1/)");
            retire=true;route="restore";
        }
        // --title-retire restore|newgame (Retire): the --title observations, continued past the Title stop with a
        // fixed state-driven pad script, to the ENTRY of 0x124548 with mode10 (restore) or mode9 (newgame).
        if(retire){
            if(title||rngProducts)throw std::runtime_error("--title-retire is exclusive with --title/--rng-products");
            if(route!="restore"&&route!="newgame")throw std::runtime_error("--title-retire takes restore or newgame");
            title=true;
        }
        const int32 routeMode=route=="restore"?10:9;
        if(title&&!minimal)rngProducts=true;
        auto bpp=[&](uint32 psm){return title?bitsPerPixelTitle(psm):bitsPerPixel(psm);};
        if(!std::filesystem::is_regular_file(disc))throw std::runtime_error("Disc input missing");
        if(std::filesystem::exists(outDir)||std::filesystem::is_symlink(outDir))throw std::runtime_error("Output directory must be fresh");
        std::vector<uint8> elf;
        {std::ifstream in(elfPath,std::ios::binary);if(!in)throw std::runtime_error("Cannot read ELF");
         elf.assign(std::istreambuf_iterator<char>(in),{});}
        if(sha(elf.data(),elf.size())!=pinnedElf)throw std::runtime_error("ELF is not the pinned retail image");
        // Restore isolated research memory cards: both ports must be existing, empty, non-symlink directories.
        auto cardListing=[](const std::filesystem::path& dir){
            std::ostringstream o;o<<"{\"path\":\""<<esc(dir.string())<<"\",\"entries\":[";bool first=true;
            std::vector<std::filesystem::path> all;
            for(const auto& e:std::filesystem::recursive_directory_iterator(dir))all.push_back(e.path());
            std::sort(all.begin(),all.end());
            for(const auto& p:all){
                o<<(first?"":",")<<"{\"rel\":\""<<esc(std::filesystem::relative(p,dir).string())<<"\",\"type\":\""
                 <<(std::filesystem::is_symlink(p)?"symlink":std::filesystem::is_directory(p)?"dir":std::filesystem::is_regular_file(p)?"file":"other")<<"\"";
                if(std::filesystem::is_regular_file(p)&&!std::filesystem::is_symlink(p)){
                    std::ifstream in(p,std::ios::binary);std::vector<uint8> b((std::istreambuf_iterator<char>(in)),{});
                    o<<",\"bytes\":"<<b.size()<<",\"sha256\":\""<<sha(b.data(),b.size())<<"\"";
                }
                o<<"}";first=false;
            }
            o<<"]}";return o.str();};
        std::filesystem::path card0,card1;
        if(restoreCancel){
            card0=std::filesystem::absolute(memcards/"mc0").lexically_normal();card1=std::filesystem::absolute(memcards/"mc1").lexically_normal();
            for(const auto& c:{card0,card1}){
                if(std::filesystem::is_symlink(c)||!std::filesystem::is_directory(c))throw std::runtime_error("memory card is not a plain directory: "+c.string());
                if(std::filesystem::directory_iterator(c)!=std::filesystem::directory_iterator())throw std::runtime_error("memory card is not empty: "+c.string());
            }
        }
        std::filesystem::create_directories(outDir);
        if(restoreCancel){
            const std::string s="{\"mc0\":"+cardListing(card0)+",\"mc1\":"+cardListing(card1)+"}\n";
            writeFresh(outDir/"memcards-before.json",s.data(),s.size());
        }

        // ---- static breakpoint plan, derived from the pinned ELF text only -------------
        auto elfWord=[&](uint32 vaddr)->uint32{
            // .text: file offset 0x1000 -> vaddr 0x100000, size 0xf8434-0x100000
            const uint32 off=vaddr-0x100000+4096;uint32 w;std::memcpy(&w,elf.data()+off,4);return w;};
        std::map<uint32,std::string> entryName;
        if(retire&&minimal){entryName[0x1296f8]="title_update";entryName[0x124548]="successor_construct";
            // Restore minimal control: plus the Restore input callback (pad script) and 0x12c350 (second-title stop).
            if(restoreCancel){entryName[0x11e480]="restore_input";entryName[0x12c350]="mode8_construct";}}
        else if(title&&minimal) entryName[0x12c350]="mode8_construct";
        else {
            for(const auto& h:hookDefs)if(h.name)entryName[h.addr]=h.name;
            if(rngProducts)for(const auto& h:rngHookDefs){
                if(entryName.count(h.addr))throw std::runtime_error("Duplicate RNG hook");
                entryName[h.addr]=h.name;
            }
            if(title)for(const auto& h:titleHookDefs){
                if(entryName.count(h.addr))throw std::runtime_error("Duplicate Title hook");
                entryName[h.addr]=h.name;
            }
            if(retire)for(const auto& h:titleRetireHookDefs){
                if(entryName.count(h.addr))throw std::runtime_error("Duplicate Retire hook");
                entryName[h.addr]=h.name;
            }
        }
        std::set<uint32> returnSites;
        for(uint32 a=0x100000;a+8<0x1f8434;a+=4) {
            const auto w=elfWord(a);
            if((w>>26)==3&&entryName.count((w&0x03ffffff)<<2))returnSites.insert(a+8);
        }
        std::map<uint32,std::string> restoreEntryName;std::set<uint32> restoreReturnSites;
        if(restoreCancel&&!minimal){
            for(const auto& h:restoreHookDefs){
                if(entryName.count(h.addr)||restoreEntryName.count(h.addr))throw std::runtime_error("Duplicate Restore hook");
                restoreEntryName[h.addr]=h.name;
            }
            for(uint32 a=0x100000;a+8<0x1f8434;a+=4){
                const auto w=elfWord(a);
                if((w>>26)==3&&restoreEntryName.count((w&0x03ffffff)<<2)&&!returnSites.count(a+8))restoreReturnSites.insert(a+8);
            }
        }
        // Top-level call sites of 0x1002f8. Title mode extends the range over the main loop body
        // (0x1005e0..0x1006c4) so every event carries the loop call that is current.
        std::map<uint32,uint32> topSites; // pc -> callee
        for(uint32 a=mainPc;a<=(title?0x1006c4u:0x100618u);a+=4) {
            const auto w=elfWord(a);if((w>>26)==3)topSites[a]=(w&0x03ffffff)<<2;
        }
        if(!topSites.count(stopPc))throw std::runtime_error("Stop PC is not a jal in this ELF");

        // ---- VM ------------------------------------------------------------------------
        std::atomic<int> topStep{-1};
        LedgerGS* gs=nullptr;
        ResearchVM vm;
        CAppConfig::GetInstance().SetPreferencePath(PREF_PS2_CDROM0_PATH,disc.string());
        if(restoreCancel){
            // Isolated research memory cards (Iop::CMcServ reads these preference paths at each operation).
            CAppConfig::GetInstance().SetPreferencePath(PREF_PS2_MC0_DIRECTORY,fs::path(card0.string()));
            CAppConfig::GetInstance().SetPreferencePath(PREF_PS2_MC1_DIRECTORY,fs::path(card1.string()));
            if(CAppConfig::GetInstance().GetPreferencePath(PREF_PS2_MC0_DIRECTORY).string()!=card0.string()
               ||CAppConfig::GetInstance().GetPreferencePath(PREF_PS2_MC1_DIRECTORY).string()!=card1.string())
                throw std::runtime_error("memory card preference paths not applied");
        }
        vm.Initialize();
        struct Cleanup { CPS2VM& vm; ~Cleanup(){vm.Pause();vm.DestroySoundHandler();vm.DestroyPadHandler();vm.DestroyGSHandler();vm.Destroy();} } cleanup{vm};
        vm.CreateGSHandler([&](){gs=new LedgerGS();return gs;});
        ScriptedPad* scriptedPad=nullptr;
        if(retire)vm.CreatePadHandler([&](){scriptedPad=new ScriptedPad();return scriptedPad;});
        else vm.CreatePadHandler([&](){return new RecordedPad(std::filesystem::path(),36000);});
        if(!vm.m_cdrom0)throw std::runtime_error("VM did not mount the disc");
        auto* bios=dynamic_cast<CIopBios*>(vm.m_iop->m_bios.get());
        if(!bios)throw std::runtime_error("IOP BIOS missing");
        auto traced=std::make_shared<TraceDisc>(vm.m_cdrom0,&topStep);
        for(const auto* name:{"cdrom","cdrom0","cdrom1"})bios->GetIoman()->RegisterDevice(name,traced);
        std::atomic<unsigned> frames{0};
        auto onFrame=vm.OnNewFrame.Connect([&](){++frames;});
        if(retire){if(!scriptedPad)throw std::runtime_error("scripted pad missing");scriptedPad->vblanks=&frames;}
        std::atomic<bool> exited{false};
        auto onExit=vm.m_ee->m_os->OnRequestExit.Connect([&](){exited=true;});
        vm.m_ee->m_os->BootFromCDROM();

        // ---- breakpoint bookkeeping ----------------------------------------------------
        std::map<uint32,int> bpRef;
        auto addBp=[&](uint32 a){if(bpRef[a]++==0)vm.m_ee->m_EE.ToggleBreakpoint(a);};
        auto delBp=[&](uint32 a){auto it=bpRef.find(a);if(it==bpRef.end()||it->second<=0)throw std::runtime_error("bp refcount");
            if(--it->second==0)vm.m_ee->m_EE.ToggleBreakpoint(a);};
        addBp(stopPc);
        bool restoreArmed=false;
        std::function<void()> armRestoreHooks=[&]{
            if(restoreArmed)throw std::runtime_error("Restore hooks armed twice");
            restoreArmed=true;
            for(const auto& kv:restoreEntryName){entryName[kv.first]=kv.second;addBp(kv.first);}
            for(auto a:restoreReturnSites){returnSites.insert(a);addBp(a);}
            for(const auto& pr:restoreProbeDefs)addBp(pr.addr);
        };
        if(!minimal) {
            addBp(randPc);
            for(const auto& kv:entryName)addBp(kv.first);
            for(auto a:returnSites)addBp(a);
            for(const auto& kv:topSites)addBp(kv.first);
            if(retire)for(const auto& pr:titleRetireProbeDefs)addBp(pr.addr);
        } else if(title) {
            for(const auto& kv:entryName)addBp(kv.first); // 0x12c350 only; its return is a dynamic breakpoint
        }

        std::atomic<bool> stateChanged{false},finishing{false};
        std::mutex stateMutex;std::condition_variable stateWake;
        auto onState=vm.OnMachineStateChange.Connect([&](){
            {std::lock_guard lock(stateMutex);stateChanged=true;}
            stateWake.notify_one();
            if(!finishing&&vm.GetStatus()==CVirtualMachine::PAUSED)vm.wait_for_observer_command();
        });

        // ---- observation state ---------------------------------------------------------
        uint8* const ram=vm.m_ee->m_ram;
        auto& cpu=vm.m_ee->m_EE.m_State;
        auto get32=[&](uint32 a){a&=0x1ffffff;uint32 v;std::memcpy(&v,ram+a,4);return v;};
        auto reg=[&](unsigned r){return uint64(cpu.nGPR[r].nD0);};
        auto cstr=[&](uint32 a){std::string s;a&=0x1ffffff;for(unsigned i=0;i<200&&a+i<PS2::EE_RAM_SIZE&&ram[a+i];++i)s.push_back(char(ram[a+i]));return s;};
        auto plausiblePtr=[&](uint64 p,uint64 n){return p>=0x100000&&p+n<=PS2::EE_RAM_SIZE&&p+n>=p;};
        auto norm=[](uint64 p){return uint64(uint32(p)&0x1fffffff);};
        std::ofstream events(outDir/"events.jsonl",std::ios::binary);
        if(!events)throw std::runtime_error("Cannot create events.jsonl");
        std::vector<Frame> stack;
        std::map<uint64,Alloc> allocs; // by allocation id
        std::vector<std::string> evLines;
        uint64 evCount=0,allocSerial=0;
        bool stopped=false,titleStopped=false,prefixPassed=false;uint32 titleStopRa=0;uint64 titleStopSp=0;
        std::string fillReport="null";
        struct FileInfo{std::string name;uint32 ptr=0;uint64 size=0;std::string sha;};
        FileInfo lastFile;
        auto randState=[&]{
            const uint32 p=get32(0x2151b4);
            std::ostringstream o;o<<"\"rand_state_214ec8_58\":"<<get32(0x214ec8+0x58)<<",\"rand_ptr_2151b4\":"<<p;
            if(plausiblePtr(p,0x5c))o<<",\"rand_state_via_ptr\":"<<get32(p+0x58);
            return o.str();
        };
        auto descJson=[&](uint64 d){
            std::ostringstream o;const uint32 hi=uint32(d>>32);
            o<<"{\"raw\":"<<hx(d)<<",\"tbp\":"<<(d&0x3fff)<<",\"tbw\":"<<((d>>14)&0x3f)<<",\"id\":"<<((d>>20)&0xfff)
             <<",\"psm\":"<<(hi&0x3f)<<",\"w\":"<<((hi>>6)&0xfff)<<",\"h\":"<<((hi>>18)&0xfff)<<"}";
            return o.str();
        };
        // RNG helpers. EFD definition record (0x30 header + n*0x74 elements): +0 id, +8 flags, +0x10 n,
        // +0x14 elements (relocated by 0x10ba38 before 0x10b7a0). Template table at *0x1f849c, slot = def id + 0x54.
        auto defJson=[&](uint64 p){
            const uint32 d=uint32(norm(p));std::ostringstream q;
            if(plausiblePtr(d,0x30))q<<"{\"ptr\":"<<hx(d)<<",\"id\":"<<get32(d)<<",\"flags\":"<<hx(get32(d+8))
                <<",\"elements\":"<<get32(d+0x10)<<",\"element_ptr\":"<<hx(get32(d+0x14))<<"}";
            else q<<"null";
            return q.str();
        };
        auto slotJob=[&](uint32 slot)->uint32{const uint32 t=get32(templateTablePtr);
            return slot<0x1000&&plausiblePtr(t,(slot+1)*4)?get32(t+slot*4):0xffffffffu;};
        auto jobJson=[&](uint32 slot){
            const uint32 j=slotJob(slot);std::ostringstream q;q<<"{\"slot\":"<<slot<<",\"job\":"<<hx(j);
            if(j!=0xffffffffu&&plausiblePtr(j,16))q<<",\"count\":"<<get32(j+8)<<",\"elements_ptr\":"<<hx(get32(j+12));
            q<<"}";return q.str();
        };
        std::atomic<uint64> evAtomic{0};
        auto emit=[&](const std::string& body){
            std::string line="{\"i\":"+std::to_string(evCount++)+",\"top\":"+std::to_string(topStep.load())+","
                             +(title?"\"vb\":"+std::to_string(frames.load())+",":std::string())+body+"}\n";
            evAtomic.store(evCount);
            events.write(line.data(),std::streamsize(line.size()));
        };
        // ---- Title helpers (title mode) ----------------------------------------------------
        auto s16=[&](uint32 a){int16 v;std::memcpy(&v,ram+(a&0x1ffffff),2);return int(v);};
        auto handleRecord=[&](int h){return (h>=0&&h<0x1000)?s16(0x3a5b00+h*2):-1;};
        auto descRefs=[&](uint64 d){return d?s16(0x3adc20+uint32((d>>20)&0xfff)*6):0;};
        auto queueIds=[&](int head,int tail){ // handle node pool H lists (node = id+8)
            std::vector<int> ids;int e=s16(0x3a1ad0+head*4+2);
            for(int n=0;e!=tail&&n<=0x1008;++n){if(e<8||e>=0x1008)throw std::runtime_error("reserved queue node");ids.push_back(e-8);e=s16(0x3a1ad0+e*4+2);}
            return ids;};
        auto intList=[](const std::vector<int>& v){std::string s="[";for(size_t i=0;i<v.size();++i)s+=(i?",":"")+std::to_string(v[i]);return s+"]";};
        auto imageList=[&]{std::vector<int> v;const int n=int(get32(imageListCount));
            for(int i=0;i<n&&i<64;++i)v.push_back(int32(get32(imageListBase+i*4)));return v;};
        auto queueContext=[&]{
            // Before 0x1cbdf8 has built the handle node pool the queues are not lists yet: reported as null.
            auto q=[&](int h,int t){try{return intList(queueIds(h,t));}catch(const std::runtime_error&){return std::string("null");}};
            std::ostringstream o;o<<",\"reserve_class\":"<<int32(get32(reserveClass))<<",\"reserved0\":"<<q(2,4)
                                  <<",\"reserved1\":"<<q(3,5)<<",\"cache_switch\":"<<int32(get32(cacheSwitch))
                                  <<",\"lower_switch\":"<<int32(get32(lowerSwitch));
            return o.str();};
        auto storageJson=[&]{std::ostringstream o;o<<",\"storage_words\":[";
            for(int i=0;i<10;++i)o<<(i?",":"")<<int32(get32(storageState+i*4));
            o<<"],\"storage_bytes_2597c8\":["<<unsigned(ram[0x2597c8])<<","<<unsigned(ram[0x2597c9])<<","<<unsigned(ram[0x2597ca])<<"]";return o.str();};
        auto materialTable=[&](uint32 t){std::ostringstream o;
            if(!plausiblePtr(t,0x10)){o<<"null";return o.str();}
            const int n=int32(get32(t+0xc));
            o<<"{\"ptr\":"<<hx(t)<<",\"next\":"<<hx(get32(t))<<",\"word1\":"<<int32(get32(t+4))<<",\"word2\":"<<int32(get32(t+8))<<",\"count\":"<<n<<",\"handles\":[";
            for(int i=0;i<n&&i<256&&plausiblePtr(t+0x10+i*4,4);++i)o<<(i?",":"")<<int32(get32(t+0x10+i*4));
            o<<"]}";return o.str();};
        auto xmdNames=[&](uint32 x){std::ostringstream o;
            if(!plausiblePtr(x,8)){o<<"null";return o.str();}
            const int32 off=int32(get32(x+4));if(off<=0||!plausiblePtr(x+off,4)){o<<"[]";return o.str();}
            const int n=int32(get32(x+off));uint32 p=x+off+4;o<<"[";
            for(int i=0;i<n&&i<1024;++i){const std::string s=cstr(p);o<<(i?",":"")<<"\""<<esc(s)<<"\"";p+=uint32(s.size())+1;}
            o<<"]";return o.str();};
        auto argsJson=[&](const uint64* a){
            std::ostringstream o;o<<"\"args\":[";
            for(int i=0;i<8;++i)o<<(i?",":"")<<hx(a[i]);o<<"]";return o.str();
        };
        static const unsigned argRegs[8]={CMIPS::A0,CMIPS::A1,CMIPS::A2,CMIPS::A3,CMIPS::T0,CMIPS::T1,CMIPS::T2,CMIPS::T3};
        // ---- Retire helpers (--title-retire) ------------------------------------------------
        bool retireStopped=false;
        int32 titleEffectSlot=-1;uint64 titleUpdates=0;int padStep=0;uint32 confirmMask=0;
        std::vector<std::string> checkpointsTaken;
        std::function<void(const std::string&)> checkpoint; // assigned after buildState
        auto cpRetire=[&](const std::string& n){return restoreCancel?"retire-"+n:n;};
        auto taken=[&](const std::string& n){return std::find(checkpointsTaken.begin(),checkpointsTaken.end(),n)!=checkpointsTaken.end();};
        bool successorSeen=false,restoreStopArmed=false;uint64 restoreInputCalls=0,uiPlaceCalls=0;
        std::vector<std::string> missingCheckpoints;
        auto effectJob=[&](int32 slot)->uint32{
            const uint32 t=get32(effectTablePtr);
            return slot>=0&&uint32(slot)<effectSlots&&plausiblePtr(t,effectSlots*4)?get32(t+uint32(slot)*4):0xffffffffu;};
        auto effectSlotOf=[&](uint32 job)->int32{
            const uint32 t=get32(effectTablePtr);if(!plausiblePtr(t,effectSlots*4))return -1;
            for(uint32 s=0;s<effectSlots;++s)if(get32(t+s*4)==job)return int32(s);return -1;};
        auto effectJson=[&](int32 slot){
            std::ostringstream q;
            if(slot<0||uint32(slot)>=effectSlots){q<<"null";return q.str();}
            const uint32 j=effectJob(slot);
            q<<"{\"slot\":"<<slot<<",\"job\":";
            if(j==0xffffffffu||!plausiblePtr(j,0x80)){q<<(j==0xffffffffu?"-1":hx(j))<<"}";return q.str();}
            q<<hx(j)<<",\"type\":"<<unsigned(ram[j&0x1ffffff])<<",\"flags\":"<<hx(get32(j+4))<<",\"elements\":"<<int32(get32(j+8))
             <<",\"rgba_40\":["<<hx(get32(j+0x40))<<","<<hx(get32(j+0x44))<<","<<hx(get32(j+0x48))<<","<<hx(get32(j+0x4c))<<"]"
             <<",\"w74\":"<<int32(get32(j+0x74));
            const uint32 e=get32(j+0xc);const int n=int32(get32(j+8));
            q<<",\"element_handles\":[";
            for(int k=0;k<n&&k<64&&plausiblePtr(e+uint32(k)*0x80,0x80);++k)q<<(k?",":"")<<s16(e+uint32(k)*0x80+0x5c);
            q<<"]}";return q.str();};
        auto instanceJson=[&](int32 idx){ // title instance table *0x21301c, 0x3c-byte entries
            std::ostringstream q;const uint32 b=get32(0x21301c);
            if(idx<0||!plausiblePtr(b+uint32(idx)*0x3c,0x3c)){q<<"null";return q.str();}
            const uint32 e=b+uint32(idx)*0x3c;
            q<<"{\"entry\":"<<hx(e)<<",\"h0\":"<<s16(e)<<",\"h2\":"<<s16(e+2)<<",\"w30\":"<<hx(get32(e+0x30))<<"}";return q.str();};
        auto nodeKind=[&](uint32 node)->std::string{
            const uint32 d=get32(node+8);
            return d==0x12c8d8?"mode8":d==0x101058?"resource":d==0x124628?"mode9_10":"other";};
        auto setPad=[&](uint32 buttons,const std::string& why){
            scriptedPad->buttons.store(buttons);
            emit("\"ev\":\"pad_script\",\"step\":"+std::to_string(padStep)+",\"buttons\":"+std::to_string(buttons)
                 +",\"title_n\":"+std::to_string(titleUpdates)+",\"why\":\""+why+"\"");
        };
        auto titleScript=[&](uint32 obj){
            const int32 sub=int32(get32(obj+0x2c)),sel=int32(get32(obj+0x38));const uint32 p=get32(obj+0x28)&0xffff;
            if(sub==0xc||sub==0xd)throw std::runtime_error("title timed out to attract (submode 0xc/0xd): pad script failed");
            if(titleUpdates==0){
                confirmMask=get32(padMaskTable+0x14);
                if(confirmMask!=0x40)throw std::runtime_error("confirm mask 0x1f85dc is not 0x40 (cross): fixed script does not apply");
                if(get32(padMaskTable+0x3c)!=0x8000)throw std::runtime_error("0x1f8604 (selection-decrement key) is not 0x8000 (left)");
                checkpoint(cpRetire("first-update"));
            }
            switch(padStep){
            case 0: if(sub==10){padStep=1;setPad(1u<<btnStart,"submode 10 accepts Start: press Start");}break;
            case 1: if(sub==10&&(p&0x800)){checkpoint(cpRetire("stable-menu"));padStep=2;setPad(0,"Start consumed by submode 10: release");}break;
            case 2: if(sub==0x1e){
                        if(sel!=1)throw std::runtime_error("menu default selection is not 1 (Restore)");
                        if(route=="restore"){padStep=4;setPad(1u<<btnCross,"menu submode 0x1e, selection 1: press confirm");}
                        else{padStep=3;setPad(1u<<btnLeft,"menu submode 0x1e, selection 1: press Left");}
                    }break;
            case 3: if(sub==0x1e&&get32(dirFlags+0xc)==1){padStep=5;setPad(0,"Left consumed (0x259970=1): release");}break;
            case 5: if(sub==0x1e&&sel==0&&get32(dirFlags+0xc)==0){padStep=4;setPad(1u<<btnCross,"selection 0 (New Game): press confirm");}break;
            case 4: if(sub==0x1e&&(p&confirmMask)){
                        if(sel!=(route=="restore"?1:0))throw std::runtime_error("confirm consumed with the wrong selection");
                        padStep=6;setPad(0,"confirm consumed by submode 0x1e: release");}break;
            default: break;
            }
            ++titleUpdates;
        };

        // ---- Restore helpers (--restore-cancel) ------------------------------------------------------
        auto nodeKindRestore=[&](uint32 node)->std::string{return get32(node+8)==0x120b48?"manager":nodeKind(node);};
        // Owner of a scheduler node: mode-8 nodes own themselves (+0x24 selector); mode9/10 wrappers carry +0x24 = mode,
        // +0x28 = parent +0x44; resource and manager nodes belong to their parent.
        auto ownerJson=[&](uint32 node){
            std::ostringstream q;const std::string k=nodeKindRestore(node);
            uint32 own=(k=="resource"||k=="manager")&&plausiblePtr(get32(node+0x14),0x30)?get32(node+0x14):node;
            const std::string ok=nodeKindRestore(own);
            int32 mode=-1,sel=-1;
            if(ok=="mode8"){mode=8;sel=int32(get32(own+0x24));}
            else if(ok=="mode9_10"){mode=int32(get32(own+0x24));sel=int32(get32(own+0x28));}
            q<<"\"owner\":{\"node_kind\":\""<<k<<"\",\"owner_node\":"<<hx(own)<<",\"owner_kind\":\""<<ok<<"\",\"owner_mode\":"<<mode<<",\"owner_selector\":"<<sel<<"}";
            return q.str();};
        auto themeJson=[&]{std::ostringstream q;q<<"\"theme_words_2d9a60\":["<<int32(get32(0x2d9a60))<<","<<int32(get32(0x2d9a64))<<","<<int32(get32(0x2d9a68))<<"]";return q.str();};
        auto managerWindows=[&](uint32 node){
            std::ostringstream q;const int32 n=int32(get32(node+0xe40));q<<"\"windows\":"<<n<<",\"window_handles\":[";
            for(int32 i=0;i<n&&i<16;++i)q<<(i?",":"")<<int32(get32(node+0x40+uint32(i)*0xe0));
            q<<"]";return q.str();};
        // UI window registry *0x200688[handle] (0x1a4e50 reads opacity +0x270; 0x1a4d70 writes +0x270..+0x27c).
        auto windowFields=[&](uint32 w){
            std::ostringstream q;
            if(!plausiblePtr(w,0x3c00)){q<<"null";return q.str();}
            q<<"{\"ptr\":"<<hx(w)<<",\"opacity_270\":"<<hx(get32(w+0x270))<<",\"max_274\":"<<hx(get32(w+0x274))
             <<",\"min_278\":"<<hx(get32(w+0x278))<<",\"speed_27c\":"<<hx(get32(w+0x27c))<<",\"mode_3bec\":"<<int32(get32(w+0x3bec))
             <<",\"w3bf0\":"<<hx(get32(w+0x3bf0))<<",\"w2698\":"<<hx(get32(w+0x2698))<<",\"w4f8\":"<<hx(get32(w+0x4f8))
             <<",\"font_20c\":"<<int32(get32(w+0x20c))<<",\"atlas_8\":"<<int32(get32(w+8))<<"}";
            return q.str();};
        auto uiWindowJson=[&](int32 h){
            const uint32 w=(h>=0&&h<64)?get32(0x200688+uint32(h)*4):0;
            return "\"ui_window\":"+(plausiblePtr(w,0x3c00)?"{\"handle\":"+std::to_string(h)+",\"fields\":"+windowFields(w)+"}":std::string("null"));};
        // O10: font entries *(*0x2003ec + 0x3c*i) (flag 0x100 = glyph-cache font) and glyph-cache LRU entries
        // *0x200484 + 0x14*k, k < 6 + *0x200488 (+8 >= 0 = a cached glyph that 0x197a98 would transfer).
        auto glyphStateJson=[&]{
            std::ostringstream q;const uint32 fb=get32(0x2003ec),cb=get32(0x200484);const int32 n=6+int32(get32(0x200488));
            q<<"\"font_entries_2003ec\":"<<hx(fb)<<",\"font_flag_words\":[";
            for(int i=0;i<8;++i)q<<(i?",":"")<<(plausiblePtr(fb+uint32(i)*0x3c,4)?hx(get32(fb+uint32(i)*0x3c)):std::string("null"));
            q<<"],\"cache_200484\":"<<hx(cb)<<",\"cache_entries\":"<<n<<",\"cache_plus8_nonneg\":";
            int nn=0;std::string first="null";
            for(int32 k=0;k<n&&k<4096&&plausiblePtr(cb+uint32(k)*0x14,0x14);++k){const int v=s16(cb+uint32(k)*0x14+8);if(v>=0){if(!nn)first=std::to_string(k);++nn;}}
            q<<nn<<",\"cache_first_nonneg\":"<<first;
            return q.str();};
        auto inputGateOpen=[&](uint32 node){
            const int32 h=int32(get32(node+0x40+get32(node+0xe48)*0xe0));
            const uint32 w=(h>=0&&h<64)?get32(0x200688+uint32(h)*4):0;
            if(!plausiblePtr(w,0x280))return false;
            float cur,prev;const uint32 cb=get32(w+0x270),pb=get32(0x24ecc4);std::memcpy(&cur,&cb,4);std::memcpy(&prev,&pb,4);
            return cur!=0.0f&&cur==prev;};
        auto restoreJson=[&](uint32 node){
            std::ostringstream q;
            q<<"\"restore\":{\"state_1f89a8\":"<<int32(get32(restoreState))<<",\"selection_1f89ac\":"<<int32(get32(restoreSel))
             <<",\"kind_1f89a0\":"<<int32(get32(restoreKind))<<",\"slot_valid_24ee40\":["<<int32(get32(0x24ee40))<<","<<int32(get32(0x24ee44))<<","<<int32(get32(0x24ee48))
             <<"],\"w24ee38\":"<<int32(get32(0x24ee38))<<",\"w24ee78\":"<<int32(get32(0x24ee78))<<",\"w24ee7c\":"<<int32(get32(0x24ee7c))
             <<",\"w24eea8\":"<<int32(get32(0x24eea8))<<",\"w24ecc0\":"<<int32(get32(0x24ecc0))
             <<",\"repeat_delay_24ecd8\":"<<int32(get32(0x24ecd8))<<",\"coroutine_24ee9a\":"<<unsigned(get32(0x24ee98)>>16)
             <<",\"manager_flags\":"<<hx(get32(node+0x1c))<<",\"e48\":"<<int32(get32(node+0xe48))<<",\"e8c\":"<<int32(get32(node+0xe8c))
             <<",\"prev_opacity_24ecc4\":"<<hx(get32(0x24ecc4))<<","<<managerWindows(node)<<","<<uiWindowJson(int32(get32(node+0x40)))<<"}"<<storageJson();
            return q.str();};
        auto wrapperJson=[&](uint32 node){
            std::ostringstream q;const int32 h=int32(get32(node+0x30));
            q<<"\"wrapper\":{\"node\":"<<hx(node)<<",\"w20_resource\":"<<hx(get32(node+0x20))<<",\"w24_mode\":"<<int32(get32(node+0x24))
             <<",\"w28_selector\":"<<int32(get32(node+0x28))<<",\"w2c_phase\":"<<int32(get32(node+0x2c))<<",\"w30_atlas_handle\":"<<h
             <<",\"atlas_record\":"<<handleRecord(h)<<",\"w34_manager\":"<<hx(get32(node+0x34))<<",\"atlas_global_24f730\":"<<int32(get32(restoreAtlasGlobal))<<"}";
            return q.str();};
        auto strArg=[&](uint64 p){return p&&plausiblePtr(uint32(p),1)?"\""+esc(cstr(uint32(p)))+"\"":std::string("null");};
        auto restoreEnterDecode=[&](const std::string& name,Frame& f,std::ostringstream& o){
            const uint64* a=f.a;
            if(name=="restore_input"||name=="restore_update"){f.aux=int32(get32(restoreState));o<<","<<restoreJson(uint32(a[0]))<<",\"call\":"<<restoreInputCalls
                <<",\"input_gate_open\":"<<(inputGateOpen(uint32(a[0]))?"true":"false");}
            else if(name=="manager_input_pass"||name=="manager_update_pass"||name=="manager_destroy")o<<",\"node\":"<<hx(uint32(a[0]))<<","<<managerWindows(uint32(a[0]))
                <<",\"flags\":"<<hx(get32(uint32(a[0])+0x1c))<<",\"state_1f89a8\":"<<int32(get32(restoreState));
            else if(name=="manager_create")o<<",\"parent\":"<<hx(uint32(a[0]))<<",\"def\":"<<hx(uint32(a[1]));
            else if(name=="window_destroy"||name=="ui_release_1a9888"||name=="ui_release_199e88")o<<",\"value\":"<<int32(a[0]);
            else if(name=="wrapper_destroy"||name=="wrapper_cleanup")o<<","<<wrapperJson(uint32(a[0]))<<",\"effect\":"<<effectJson(titleEffectSlot)<<","<<themeJson();
            else if(name=="atlas_load")o<<",\"str\":"<<strArg(a[0]);
            else if(name=="ruw_load")o<<",\"str\":"<<strArg(a[0])<<",\"a1\":"<<int32(a[1]);
            else if(name=="restore_select")o<<",\"value\":"<<int32(a[0]);
            else if(name=="resource_tick"||name=="resource_construct"){
                f.aux2=uint32(uiPlaceCalls);
                o<<",\"node\":"<<hx(uint32(a[0]))<<","<<ownerJson(uint32(a[0]))<<",\"effect\":"<<effectJson(titleEffectSlot)<<","<<themeJson();}
            else if(name=="ui_place_19c568"||name=="ui_place_19ca20"){++uiPlaceCalls;o<<",\"place_n\":"<<uiPlaceCalls;}
            else if(name=="ui_pass")f.aux2=uint32(uiPlaceCalls);
            else if(name=="theme_tick"||name=="theme_end_12de98"||name=="theme_release_12dcb0"||name=="audio_12d470")o<<","<<themeJson();
            else if(name.rfind("mc_",0)==0)o<<storageJson();
            else if(name=="window_fade_out"||name=="window_fade_in")o<<",\"f12_bits\":"<<hx(cpu.nCOP1[12])<<",\"record\":"<<hx(uint32(a[0]));
            else if(name=="wrapper_tick")o<<","<<wrapperJson(uint32(a[0]));
            else if(name=="window_opacity_step"||name=="ui_layout"||name=="ui_layout_19d0c8"||name=="ui_layout_mode0"||name=="ui_layout_mode2")
                o<<",\"window\":"<<windowFields(uint32(a[0]));
            else if(name=="glyph_cache_lookup"||name=="glyph_path_195ff0")o<<","<<glyphStateJson();
        };
        auto restoreExitDecode=[&](const std::string& name,const Frame& f,std::ostringstream& o){
            const uint64 v0=reg(CMIPS::V0);
            if(name=="successor_construct")o<<","<<wrapperJson(uint32(f.a[0]))<<",\"kind_1f89a0\":"<<int32(get32(restoreKind))
                <<",\"state_1f89a8\":"<<int32(get32(restoreState));
            else if(name=="restore_input"||name=="restore_update")o<<",\"state_entry\":"<<f.aux<<","<<restoreJson(uint32(f.a[0]));
            else if(name=="manager_construct")o<<",\"node\":"<<hx(uint32(f.a[0]))<<","<<managerWindows(uint32(f.a[0]));
            else if(name=="manager_create"||name=="window_construct")o<<",\"result\":"<<hx(uint32(v0));
            else if(name=="atlas_load")o<<",\"handle\":"<<int32(v0)<<",\"record\":"<<handleRecord(int32(v0))<<",\"atlas_global_24f730\":"<<int32(get32(restoreAtlasGlobal));
            else if(name.rfind("mc_",0)==0)o<<",\"result\":"<<int64(v0)<<storageJson();
            else if(name=="resource_tick")o<<",\"effect\":"<<effectJson(titleEffectSlot)<<","<<themeJson()<<",\"place_calls_inside\":"<<(uiPlaceCalls-f.aux2);
            else if(name=="wrapper_destroy"||name=="wrapper_cleanup")o<<",\"effect\":"<<effectJson(titleEffectSlot)<<","<<themeJson();
            else if(name=="theme_tick"||name=="theme_end_12de98"||name=="theme_release_12dcb0")o<<","<<themeJson();
            else if(name=="ui_pass")o<<",\"place_calls_inside\":"<<(uiPlaceCalls-f.aux2);
            else if(name=="opacity_get")o<<",\"f0_bits\":"<<hx(cpu.nCOP1[0])<<",\"caller_ra\":"<<hx(f.ra);
            else if(name=="manager_update_pass"||name=="manager_input_pass")o<<",\"state_1f89a8\":"<<int32(get32(restoreState))
                <<",\"flags\":"<<hx(get32(uint32(f.a[0])+0x1c));
            else if(name=="wrapper_tick")o<<","<<wrapperJson(uint32(f.a[0]));
            else if(name=="mode_parent_tick"){const uint32 c=get32(uint32(f.a[0])+0x38);
                o<<",\"child_flags\":"<<(plausiblePtr(c,0x20)?hx(get32(c+0x1c)):std::string("null"))<<",\"child_kind\":\""<<(plausiblePtr(c,0x20)?nodeKindRestore(c):"none")<<"\"";}
            else if(name=="glyph_cache_lookup"||name=="glyph_path_195ff0")o<<","<<glyphStateJson();
        };
        auto restoreScriptEnter=[&](uint32 node){
            const int32 st=int32(get32(restoreState));
            if(padStep==6&&st==0x226)throw std::runtime_error("Restore found saved data (state 0x226): the research card is not empty");
            if(padStep==6&&st==0x21c&&inputGateOpen(node)){
                if(get32(cancelMaskAddr)!=0x20)throw std::runtime_error("cancel mask 0x1f85e0 is not 0x20 (circle): fixed script does not apply");
                if(get32(restoreKind)!=1)throw std::runtime_error("0x1f89a0 is not 1 (Restore)");
                checkpoint("empty-ready");
                padStep=7;setPad(1u<<btnCircle,"Restore empty-slot state 0x21c accepts Cancel: press cancel (0x1f85e0)");
            }
            (void)node;++restoreInputCalls;
        };
        auto restoreScriptExit=[&](int32 entryState){
            const int32 st=int32(get32(restoreState));
            if(padStep==7&&entryState==0x21c&&st==-1){padStep=8;setPad(0,"Cancel consumed (0x21c -> -1 in this 0x11e480 call): release");}
        };

        // Entry-side decoding of hooked functions (reads only).
        auto onEnter=[&](uint32 pc){
            Frame f{};f.fn=pc;f.name=entryName[pc].c_str();f.ra=uint32(reg(CMIPS::RA));f.sp=reg(CMIPS::SP);
            for(int i=0;i<8;++i)f.a[i]=reg(argRegs[i]);
            const std::string name=entryName[pc];
            std::ostringstream o;
            o<<"\"ev\":\"enter\",\"fn\":"<<hx(pc)<<",\"name\":\""<<name<<"\",\"depth\":"<<stack.size()
             <<",\"parent\":"<<(stack.empty()?std::string("null"):"\""+std::string(stack.back().name)+"\"")
             <<",\"ra\":"<<hx(f.ra)<<",\"sp\":"<<hx(f.sp)<<","<<argsJson(f.a);
            const auto& a=f.a;
            if(name=="alloc") o<<",\"class\":"<<int32(a[0])<<",\"psm\":"<<int32(a[1])<<",\"w\":"<<int32(a[2])<<",\"h\":"<<int32(a[3]);
            else if(name=="retain"||name=="free"||name=="handle_create") {
                o<<",\"desc\":"<<descJson(a[0]);
                if(name=="retain")o<<",\"delta\":"<<int32(a[1]);
            }
            else if(name=="upload") {
                const uint32 psm=uint32(a[0]>>32)&0x3f;const uint64 w=uint32(a[3]),h=uint32(a[4]),src=norm(a[5]);
                o<<",\"desc\":"<<descJson(a[0])<<",\"x\":"<<int32(a[1])<<",\"y\":"<<int32(a[2])<<",\"w\":"<<w<<",\"h\":"<<h<<",\"src\":"<<hx(src);
                const int bits=bpp(psm);
                if(bits&&src) {
                    const uint64 qwc=w*h*bits/128,bytes=qwc*16;
                    o<<",\"qwc\":"<<qwc<<",\"src_bytes\":"<<bytes;
                    if(plausiblePtr(src,bytes))o<<",\"src_sha256\":\""<<sha(ram+src,bytes)<<"\"";
                    else o<<",\"src_sha256\":null";
                } else o<<",\"src_sha256\":null,\"src_note\":\""<<(src?"unsupported psm for hashing":"null source")<<"\"";
            }
            else if(name=="image_alloc_handle_upload") {
                const uint32 psm=uint32(a[0]),src=uint32(norm(a[3]));const uint64 w=uint32(a[1]),h=uint32(a[2]);
                o<<",\"psm\":"<<psm<<",\"w\":"<<w<<",\"h\":"<<h<<",\"src\":"<<hx(src);
                const int bits=bpp(psm);
                if(bits&&src&&plausiblePtr(src,w*h*bits/8))o<<",\"src_nominal_bytes\":"<<w*h*bits/8<<",\"src_nominal_sha256\":\""<<sha(ram+src,w*h*bits/8)<<"\"";
                else o<<",\"src_nominal_sha256\":null,\"src_note\":\""<<(src?"unsupported psm or range":"null source (no upload)")<<"\"";
            }
            else if(name=="clut_alloc_upload") {
                const uint64 n=uint32(a[0]);const uint32 src=uint32(norm(a[1]));
                o<<",\"colors\":"<<n<<",\"src\":"<<hx(src);
                if(src&&n&&n<=4096&&plausiblePtr(src,n*4))o<<",\"src_bytes\":"<<n*4<<",\"src_sha256\":\""<<sha(ram+src,n*4)<<"\"";
                else o<<",\"src_sha256\":null,\"src_note\":\""<<(src?"range":"null source: allocated, no upload expected")<<"\"";
            }
            else if(name=="palette_bind") o<<",\"handle\":"<<int32(a[0])<<",\"level\":"<<int32(a[1])<<",\"desc\":"<<descJson(a[2]);
            else if(name=="palette_bind_desc") o<<",\"handle\":"<<int32(a[0])<<",\"desc\":"<<descJson(a[1]);
            else if(name=="handle_retire"||name=="handle_alias") o<<",\"handle_or_record\":"<<int32(a[0]);
            else if(name=="handle_name") {
                o<<",\"handle\":"<<int32(a[0]);o<<",\"name_str\":"<<(a[1]&&plausiblePtr(uint32(a[1]),1)?"\""+esc(cstr(uint32(a[1])))+"\"":std::string("null"));
            }
            else if(name=="handle_lookup"||name=="dir_prefix"||name=="archive_mount"||name=="file_read")
                o<<",\"str\":"<<(a[0]&&plausiblePtr(uint32(a[0]),1)?"\""+esc(cstr(uint32(a[0])))+"\"":std::string("null"));
            else if(name=="file_read_raw") {
                o<<",\"str\":"<<(a[0]&&plausiblePtr(uint32(a[0]),1)?"\""+esc(cstr(uint32(a[0])))+"\"":std::string("null"));
                f.fileBuf=uint32(a[1]);lastFile=FileInfo{};lastFile.name=cstr(uint32(a[0]));lastFile.ptr=uint32(a[1]);
            }
            else if(name=="color_depth_init") o<<",\"layers_w\":"<<int32(a[1])<<",\"layers_h\":"<<int32(a[2]);
            else if(name=="font_load") o<<",\"index\":"<<int32(a[0])<<",\"buf\":"<<hx(a[1])<<",\"flags\":"<<hx(a[2]);
            else if(rngProducts) {
                if(name=="efd_import") o<<",\"efd\":"<<hx(norm(a[0]))<<",\"efd_magic\":"<<hx(get32(uint32(norm(a[0]))))<<",\"efd_count\":"<<get32(uint32(norm(a[0]))+4);
                else if(name=="efd_define"||name=="efd_reference") o<<",\"def\":"<<defJson(a[0]);
                else if(name=="efd_range") o<<",\"start\":"<<int32(a[0])<<",\"end\":"<<int32(a[1])<<",\"base\":"<<int32(a[2])<<",\"def\":"<<defJson(a[3]);
                else if(name=="template_request") {
                    const uint32 shape=uint32(a[0]);
                    o<<",\"shape\":"<<int32(a[0])<<",\"elem\":"<<int32(a[1])<<",\"texture\":"<<hx(a[2])
                     <<",\"current_slot\":"<<get32(currentSlot)<<",\"current\":"<<jobJson(get32(currentSlot));
                    if(shape<66)o<<",\"cache_def_before\":"<<get32(cacheDefTable+shape*4)<<",\"cache_elem_before\":"<<get32(cacheElemTable+shape*4);
                }
                else if(name=="template_share") o<<",\"owner_slot\":"<<int32(a[0])<<",\"owner_elem\":"<<int32(a[1])<<",\"elem\":"<<int32(a[2])
                    <<",\"owner\":"<<jobJson(uint32(a[0]))<<",\"current\":"<<jobJson(get32(currentSlot));
                else if(name.rfind("factory_",0)==0) o<<",\"current\":"<<jobJson(get32(currentSlot));
            }
            if(title) {
                auto str0=[&](uint64 p){return p&&plausiblePtr(uint32(p),1)?"\""+esc(cstr(uint32(p)))+"\"":std::string("null");};
                if(name=="mode8_construct"){
                    const uint32 obj=uint32(a[0]),parent=get32(obj+0x14);
                    const int32 sel=plausiblePtr(parent,0x48)?int32(get32(parent+0x44)):-1;
                    f.aux=sel;
                    o<<",\"selector\":"<<sel<<",\"image_list\":"<<intList(imageList())<<storageJson()<<queueContext();
                }
                else if(name=="logo_tick"){f.aux=int32(get32(uint32(a[0])+0x30))+1;o<<",\"tick\":"<<f.aux;}
                else if(name=="title_update"&&!retire)throw std::runtime_error("0x1296f8 (first title update) reached before the 0x12c350(selector 1) return stop");
                else if(name=="title_update"){
                    const uint32 obj=uint32(a[0]);
                    o<<",\"title_n\":"<<titleUpdates<<",\"submode\":"<<int32(get32(obj+0x2c))<<",\"counter\":"<<int32(get32(obj+0x30))
                     <<",\"selection\":"<<int32(get32(obj+0x38))<<",\"pressed_word\":"<<(get32(obj+0x28)&0xffff)
                     <<",\"fade_b8\":"<<hx(get32(obj+0xb8))<<",\"effect_slot_c8\":"<<int32(get32(obj+0xc8))<<",\"hanten_cc\":"<<int32(get32(obj+0xcc))
                     <<",\"dir_flags\":["<<int32(get32(dirFlags))<<","<<int32(get32(dirFlags+4))<<","<<int32(get32(dirFlags+8))<<","<<int32(get32(dirFlags+0xc))
                     <<"],\"dir_repeat\":"<<int32(get32(dirRepeat))<<",\"effect\":"<<effectJson(titleEffectSlot)
                     <<",\"host_buttons\":"<<(scriptedPad?scriptedPad->buttons.load():0);
                    if(titleUpdates==0){o<<",\"pad_mask_table_1f85c8\":[";for(int k=0;k<16;++k)o<<(k?",":"")<<hx(get32(padMaskTable+k*4));o<<"]";}
                }
                else if(name=="successor_construct"){
                    const uint32 node=uint32(a[0]),parent=get32(node+0x14);
                    o<<",\"node\":"<<hx(node)<<",\"descriptor_ctor\":"<<hx(get32(node+0))<<",\"parent\":"<<hx(parent)
                     <<",\"parent_40_mode\":"<<int32(get32(parent+0x40))<<",\"parent_44\":"<<int32(get32(parent+0x44))
                     <<",\"parent_48\":"<<int32(get32(parent+0x48))<<",\"parent_4c\":"<<int32(get32(parent+0x4c))<<",\"parent_50\":"<<int32(get32(parent+0x50))
                     <<",\"mode_24f738\":"<<int32(get32(0x24f738))<<",\"effect\":"<<effectJson(titleEffectSlot);
                    f.aux=int32(get32(parent+0x4c));
                }
                else if(name=="pad_pressed"||name=="pad_repeat")o<<",\"port\":"<<int32(a[0]);
                else if(name=="effect_alloc"||name=="effect_create_title")o<<",\"shape\":"<<int32(a[0]);
                else if(name=="effect_bind")o<<",\"slot\":"<<int32(a[0])<<",\"element\":"<<int32(a[1])<<",\"handle\":"<<int32(a[2])
                                             <<",\"record\":"<<handleRecord(int32(a[2]))<<",\"effect\":"<<effectJson(int32(a[0]));
                else if(name=="effect_rebind")o<<",\"slot\":"<<int32(a[0])<<",\"element\":"<<int32(a[1])<<",\"which\":"<<int32(a[2]);
                else if(name=="effect_remove_request"||name=="effect_remove_checked")o<<",\"slot\":"<<int32(a[0])<<",\"effect\":"<<effectJson(int32(a[0]));
                else if(name=="effect_service")o<<",\"effect\":"<<effectJson(titleEffectSlot);
                else if(name=="effect_free"){const uint32 p=uint32(norm(a[0]));o<<",\"ptr\":"<<hx(p)<<",\"job_slot\":"<<effectSlotOf(p);}
                else if(name=="mode_parent_tick"){const uint32 p=uint32(a[0]);
                    o<<",\"parent\":"<<hx(p)<<",\"phase\":"<<int32(get32(p+0x20))<<",\"child\":"<<hx(get32(p+0x38))<<",\"request\":"<<int32(get32(p+0x3c))
                     <<",\"current\":"<<int32(get32(p+0x40))<<",\"w44\":"<<int32(get32(p+0x44))<<",\"w48\":"<<int32(get32(p+0x48))
                     <<",\"w4c\":"<<int32(get32(p+0x4c))<<",\"w50\":"<<int32(get32(p+0x50));}
                else if(name=="mode_child_create")o<<",\"parent\":"<<hx(uint32(a[0]))<<",\"mode\":"<<int32(a[1]);
                else if(name=="node_create"){const uint32 d=uint32(a[1]);
                    o<<",\"parent\":"<<hx(uint32(a[0]))<<",\"descriptor\":"<<hx(d)<<",\"size\":"<<int32(a[2])
                     <<",\"ctor\":"<<hx(get32(d))<<",\"cb0\":"<<hx(get32(d+4))<<",\"cb1\":"<<hx(get32(d+8))<<",\"dtor\":"<<hx(get32(d+12));}
                else if(name=="mode8_destroy"){const uint32 n=uint32(a[0]);
                    o<<",\"node\":"<<hx(n)<<",\"selector\":"<<int32(get32(n+0x24))<<",\"flags\":"<<hx(get32(n+0x1c))<<",\"first_child\":"<<hx(get32(n+0x18))
                     <<",\"effect\":"<<effectJson(titleEffectSlot);}
                else if(name=="theme_retire_request")o<<",\"value\":"<<int32(a[0])<<",\"theme_words\":["<<int32(get32(0x2d9a60))<<","<<int32(get32(0x2d9a64))<<","<<int32(get32(0x2d9a68))<<"]";
                else if(name=="host_free"){const uint32 p=uint32(norm(a[0]));const uint32 ra=f.ra;
                    o<<",\"ptr\":"<<hx(p);
                    if((ra==0x1150d4||ra==0x1150f0)&&plausiblePtr(p,0x20)){
                        const uint32 par=get32(p+0x14);
                        const int32 selp=nodeKind(p)=="mode8"?int32(get32(p+0x24)):(plausiblePtr(par,0x28)&&nodeKind(par)=="mode8"?int32(get32(par+0x24)):-1);
                        o<<",\"scheduler_node\":{\"kind\":\""<<nodeKind(p)<<"\",\"dtor\":"<<hx(get32(p+8))<<",\"cb1\":"<<hx(get32(p+4))<<",\"parent\":"<<hx(par)
                         <<",\"flags\":"<<hx(get32(p+0x1c))<<",\"mode8_selector\":"<<selp<<"}";
                        if(restoreCancel&&successorSeen)o<<","<<ownerJson(p);
                    }
                }
                else if(name=="model_table_retire")o<<",\"holder\":"<<hx(norm(a[0]))<<",\"table\":"<<materialTable(get32(uint32(norm(a[0]))))
                                                    <<",\"material_list_head\":"<<hx(get32(materialListHead));
                else if(name=="model_group_retire"||name=="resource_slot_retire"||name=="title_sound")o<<",\"value\":"<<int32(a[0]);
                else if(name=="range_free")o<<",\"pool\":"<<hx(norm(a[0]))<<",\"arg1\":"<<int32(a[1]);
                else if(name=="theme_start")o<<",\"value\":"<<int32(a[0])<<",\"str\":"<<str0(a[1]);
                else if(name=="desc_refs_query")o<<",\"desc\":"<<descJson(a[0]);
                else if(name=="effect_template_bind")o<<",\"slot\":"<<int32(a[0])<<",\"element\":"<<int32(a[1])<<",\"a2\":"<<int32(a[2])<<",\"a3\":"<<int32(a[3]);
                else if(name=="instance_retire")o<<",\"index\":"<<int32(a[0])<<",\"instance\":"<<instanceJson(int32(a[0]));
                else if(name=="instance_release"||name=="image_slot_release")o<<",\"index\":"<<int32(a[0]);
                else if(name=="image_list_release_tail")o<<",\"w_337610\":"<<int32(get32(0x337610))<<",\"w_337920\":"<<int32(get32(0x337920))
                                                         <<",\"image_list\":"<<intList(imageList());
                if(retire&&name=="scene_retire")o<<",\"w_337ef0\":"<<int32(get32(0x337ef0));
                if(retire&&name=="handle_retire"){const int r=handleRecord(int32(a[0]));
                    o<<",\"record_hash_slot\":"<<(r>=0?s16(get32(0x3a7b00)+uint32(r)*0x30+0x24):-1);}
                if(retire&&name=="effect_bind"){const uint32 j=effectJob(int32(a[0]));
                    o<<",\"element_7c\":"<<(j!=0xffffffffu&&plausiblePtr(j,0x10)?hx(get32(get32(j+0xc)+uint32(a[1])*0x80+0x7c)):std::string("null"));}
                if(retire&&name=="successor_construct"){
                    o<<",\"root\":"<<hx(get32(0x1f85c0))<<",\"root_children\":[";
                    const uint32 root=get32(0x1f85c0);uint32 c=plausiblePtr(root,0x20)?get32(root+0x18):0;
                    for(int k=0;c&&k<64&&plausiblePtr(c,0x20);++k,c=get32(c+0x10))
                        o<<(k?",":"")<<"{\"node\":"<<hx(c)<<",\"cb0\":"<<hx(get32(c))<<",\"cb1\":"<<hx(get32(c+4))<<",\"dtor\":"<<hx(get32(c+8))
                         <<",\"flags\":"<<hx(get32(c+0x1c))<<",\"first_child\":"<<hx(get32(c+0x18))<<",\"prev\":"<<hx(get32(c+0xc))<<"}";
                    o<<"]";
                }
                else if(name=="mode_request")o<<",\"mode\":"<<int32(a[1])<<",\"selector\":"<<int32(a[2])<<",\"arg3\":"<<int32(a[3]);
                else if(name=="storage_init"||name=="storage_probe"){f.aux2=uint32(a[0]);o<<",\"param\":"<<hx(a[0])<<storageJson();}
                else if(name=="image_list_load"||name=="image_list_append")
                    o<<",\"image_name\":"<<str0(a[1])<<",\"image_list\":"<<intList(imageList());
                else if(name=="image_load_append")o<<",\"image_name\":"<<str0(a[0])<<",\"palette_arg\":"<<int32(a[1])<<",\"image_list\":"<<intList(imageList());
                else if(name=="image_list_retire"||name=="image_slots_retire"||name=="resource_retire")
                    o<<",\"image_list\":"<<intList(imageList())<<",\"slots_337610\":"<<int32(get32(0x337610))<<",\"count_337920\":"<<int32(get32(0x337920));
                else if(name=="xmd_load_path"||name=="file_load")o<<",\"str\":"<<str0(a[0])<<queueContext();
                else if(name=="xmd_load")o<<",\"xmd\":"<<hx(norm(a[0]))<<",\"xmd_names\":"<<xmdNames(uint32(norm(a[0])))<<queueContext();
                else if(name=="xmd_bind"||name=="xmd_model_build")o<<",\"xmd\":"<<hx(norm(a[0]))<<",\"first_handle\":"<<int32(a[1]);
                else if(name=="material_table"){f.aux2=uint32(norm(a[0]));
                    o<<",\"table\":"<<hx(f.aux2)<<",\"xmd\":"<<hx(norm(a[1]))<<",\"first_handle\":"<<int32(a[2])<<",\"slot\":"<<int32(a[3])<<",\"models\":"<<int32(a[4])
                     <<",\"material_list_head\":"<<hx(get32(materialListHead));}
                else if(name=="handle_successor")o<<",\"handle\":"<<int32(a[0])<<",\"record\":"<<handleRecord(int32(a[0]));
                else if(name=="upload_qwc")o<<",\"psm\":"<<int32(a[0])<<",\"w\":"<<int32(a[3])<<",\"h\":"<<int32(a[4]);
                else if(name=="reserve_class_set"||name=="cache_enable_set"||name=="lower_enable_set")o<<",\"value\":"<<int32(a[0])<<queueContext();
                else if(name=="range_alloc")o<<",\"pool\":"<<hx(norm(a[0]))<<",\"units\":"<<int32(a[1]);
                else if(name=="handle_reserve")o<<",\"count\":"<<int32(a[0])<<queueContext();
                else if(name=="handle_create"||name=="handle_alias"||name=="handle_lookup")o<<queueContext();
                else if(name=="handle_retire"){
                    const int h=int32(a[0]),r=handleRecord(h);
                    o<<",\"record\":"<<r<<",\"record_handles_before\":"<<(r>=0?int(get32(get32(0x3a7b00)+r*0x30+0x2c)&0xffff):-1);
                }
                else if(name=="retain")o<<",\"refs_before\":"<<descRefs(a[0]);
                else if(name=="free"){f.aux=descRefs(a[0]);f.aux2=get32(0x3adc1c);o<<",\"refs_before\":"<<f.aux<<",\"available_before\":"<<f.aux2;}
                else if(name=="xim_load")o<<",\"xim\":"<<hx(norm(a[0]))<<",\"last_raw_read\":{\"name\":\""<<esc(lastFile.name)<<"\",\"size\":"<<lastFile.size
                                           <<",\"sha256\":"<<(lastFile.sha.empty()?std::string("null"):"\""+lastFile.sha+"\"")<<"}";
                else if(name=="palette_bind")o<<",\"record\":"<<handleRecord(int32(a[0]));
            }
            if(name=="main"||name=="noise_texture"||name=="effect_noise_grid"||name=="effects_load"||name=="fonts_init"||name=="capture_filter_init"||name=="glyph_cache"||name=="effect_script_init")
                o<<","<<randState();
            if(retire&&name=="resource_retire"){
                const uint32 n=uint32(f.a[0]),par=get32(n+0x14);
                o<<",\"node\":"<<hx(n)<<",\"parent\":"<<hx(par)<<",\"mode8_selector\":"<<(plausiblePtr(par,0x28)?int32(get32(par+0x24)):-1)
                 <<",\"w20\":"<<int32(get32(n+0x20))<<",\"w34\":"<<int32(get32(n+0x34))<<",\"w48\":"<<int32(get32(n+0x48))
                 <<",\"effect\":"<<effectJson(titleEffectSlot);
                if(restoreCancel&&successorSeen)o<<","<<ownerJson(n);
            }
            if(restoreCancel&&successorSeen)restoreEnterDecode(name,f,o);
            f.enterIndex=evCount;
            emit(o.str());
            if(retire){
                // Retire checkpoints and the state-driven pad script run after the entry event is logged, while paused.
                if(name=="title_update")titleScript(uint32(f.a[0]));
                else if(name=="effect_remove_request"&&int32(f.a[0])==titleEffectSlot&&titleEffectSlot>=0
                        &&f.ra>=0x1296f8&&f.ra<0x12a330&&!successorSeen
                        &&std::find(checkpointsTaken.begin(),checkpointsTaken.end(),cpRetire("removal-request"))==checkpointsTaken.end())
                    checkpoint(cpRetire("removal-request"));
                else if(name=="mode8_destroy"&&int32(get32(uint32(f.a[0])+0x24))==1&&!successorSeen)checkpoint(cpRetire("resource-retirement"));
                else if(name=="successor_construct"&&!restoreCancel){
                    if(f.aux!=routeMode)throw std::runtime_error("0x124548 entered with mode "+std::to_string(f.aux)+", route expects "+std::to_string(routeMode));
                    checkpoint("successor-entry");
                    retireStopped=true;
                    return; // paused AT the 0x124548 entry: nothing of the successor constructor body has executed
                }
                else if(name=="successor_construct"){
                    // Restore: the Retire stop point becomes a checkpoint; arm the Restore hooks and continue into the body.
                    if(f.aux!=routeMode)throw std::runtime_error("0x124548 entered with mode "+std::to_string(f.aux)+", route expects "+std::to_string(routeMode));
                    if(successorSeen)throw std::runtime_error("0x124548 entered twice");
                    checkpoint("retire-successor-entry");
                    successorSeen=true;armRestoreHooks();
                }
                else if(restoreCancel&&successorSeen){
                    if(name=="restore_input")restoreScriptEnter(uint32(f.a[0]));
                    else if(name=="manager_destroy"){
                        if(padStep!=8)throw std::runtime_error("Restore manager destroyed before the Cancel was consumed");
                        if(!taken("cancel-death"))checkpoint("cancel-death");
                    }
                    else if(name=="wrapper_destroy"&&!taken("resource-retirement"))checkpoint("resource-retirement");
                    else if(name=="host_free"&&(f.ra==0x1150d4||f.ra==0x1150f0)&&nodeKind(uint32(norm(f.a[0])))=="mode9_10"){
                        if(!taken("resource-retirement"))throw std::runtime_error("wrapper freed before its destructor");
                        checkpoint("wrapper-free");restoreStopArmed=true;
                    }
                }
            }
            if(!returnSites.count(f.ra)) {
                f.dynamicBp=true;addBp(f.ra);
                emit("\"ev\":\"note\",\"note\":\"dynamic return breakpoint (indirect call)\",\"fn\":"+hx(pc)+",\"ra\":"+hx(f.ra));
            }
            stack.push_back(f);
        };
        auto onExitAt=[&](size_t idx){
            Frame f=stack[idx];stack.erase(stack.begin()+long(idx));
            const std::string name=f.name;
            const uint64 v0=reg(CMIPS::V0),v1=reg(CMIPS::V1);
            std::ostringstream o;
            o<<"\"ev\":\"exit\",\"fn\":"<<hx(f.fn)<<",\"name\":\""<<name<<"\",\"enter\":"<<f.enterIndex<<",\"depth\":"<<stack.size()
             <<",\"v0\":"<<hx(v0)<<",\"v1\":"<<hx(v1);
            if(name=="alloc") {
                if(v0) {
                    o<<",\"result\":"<<descJson(v0);
                    allocs[(v0>>20)&0xfff]=Alloc{int32(f.a[0]),int32(f.a[1]),int32(f.a[2]),int32(f.a[3]),v0,++allocSerial};
                    if(title)o<<",\"serial\":"<<allocSerial;
                } else o<<",\"result\":null";
            } else if(name=="clut_alloc_upload") {
                o<<",\"result\":"<<(v0?descJson(v0):std::string("null"));
            } else if(name=="handle_create"||name=="image_alloc_handle_upload"||name=="xim_load"||name=="handle_alias"||name=="handle_lookup")
                o<<",\"handle\":"<<int32(v0);
            else if(name=="file_read_raw") {
                lastFile.size=uint32(v0);
                const uint32 s2=uint32(reg(CMIPS::S2));
                o<<",\"bytes_read\":"<<int32(v0)<<",\"caller_s2\":"<<s2;
                if(int32(v0)>0&&plausiblePtr(lastFile.ptr,uint32(v0))){lastFile.sha=sha(ram+lastFile.ptr,uint32(v0));o<<",\"buf\":"<<hx(lastFile.ptr)<<",\"sha256\":\""<<lastFile.sha<<"\"";}
                else o<<",\"sha256\":null";
                o<<",\"file\":\""<<esc(lastFile.name)<<"\"";
            } else if(name=="file_read") {
                o<<",\"buf\":"<<hx(v0)<<",\"last_raw_read\":{\"name\":\""<<esc(lastFile.name)<<"\",\"size\":"<<lastFile.size<<",\"sha256\":"
                 <<(lastFile.sha.empty()?std::string("null"):"\""+lastFile.sha+"\"")<<"}";
            }
            if(name=="main"||name=="noise_texture"||name=="effect_noise_grid"||name=="effects_load"||name=="fonts_init"||name=="capture_filter_init"||name=="glyph_cache"||name=="effect_script_init")
                o<<","<<randState();
            if(rngProducts) {
                if(name=="effect_noise_grid") {
                    const uint32 g=get32(gridPtr);
                    o<<",\"grid_ptr\":"<<hx(g);
                    if(g==uint32(v0)&&plausiblePtr(g,384*4)){o<<",\"grid\":[";for(uint32 k=0;k<384;++k)o<<(k?",":"")<<get32(g+k*4);o<<"]";}
                    else o<<",\"grid\":null";
                } else if(name=="template_request"||name=="template_share"||name.rfind("factory_",0)==0)
                    o<<",\"current\":"<<jobJson(get32(currentSlot));
                else if(name=="effect_script_init") {
                    // Products of the whole 0x10bc78 pass, before anything later in startup can touch them.
                    writeFresh(outDir/"ee-ram-after-10bc78.bin",ram,0x2000000);
                    o<<",\"snapshot\":\"ee-ram-after-10bc78.bin\",\"snapshot_sha256\":\""<<sha(ram,0x2000000)<<"\"";
                }
            }
            bool stopHere=false;
            if(title) {
                if(name=="mode8_construct"){
                    const int32 sel=int32(get32(uint32(f.a[0])+0x24));
                    o<<",\"selector_entry\":"<<f.aux<<",\"selector\":"<<sel<<",\"image_list\":"<<intList(imageList())<<storageJson()<<queueContext();
                    if(sel!=f.aux)throw std::runtime_error("0x12c350 selector changed between entry and return");
                    stopHere=sel==1&&(!retire||(restoreCancel&&successorSeen));
                }
                else if(name=="pad_pressed"||name=="pad_repeat")o<<",\"word\":"<<(uint32(v0)&0xffff)<<",\"caller_ra\":"<<hx(f.ra);
                else if(name=="pad_analog"){
                    o<<",\"caller_ra\":"<<hx(f.ra)<<",\"outputs\":[";
                    for(int k=0;k<4;++k)o<<(k?",":"")<<(plausiblePtr(norm(f.a[k]),4)?std::to_string(int32(get32(uint32(norm(f.a[k]))))):std::string("null"));
                    o<<"]";
                }
                else if(name=="title_instance_create")o<<",\"index\":"<<int32(v0)<<",\"instance\":"<<instanceJson(int32(v0));
                else if(name=="desc_refs_query")o<<",\"result\":"<<int32(v0);
                else if(name=="effect_alloc"){
                    o<<",\"slot\":"<<int32(v0)<<",\"effect\":"<<effectJson(int32(v0));
                    const uint32 j=effectJob(int32(v0));
                    if(j!=0xffffffffu&&plausiblePtr(j,0x80)){
                        o<<",\"job_50_5f\":["<<hx(get32(j+0x50))<<","<<hx(get32(j+0x54))<<","<<hx(get32(j+0x58))<<","<<hx(get32(j+0x5c))
                         <<"],\"job_68\":"<<hx(get32(j+0x68));
                        const uint32 e=get32(j+0xc);if(plausiblePtr(e,0x80))o<<",\"element0_7c\":"<<hx(get32(e+0x7c));
                    }
                }
                else if(name=="effect_create_title"){
                    titleEffectSlot=int32(v0);o<<",\"slot\":"<<int32(v0)<<",\"effect\":"<<effectJson(int32(v0));
                }
                else if(name=="effect_bind"||name=="effect_rebind"||name=="effect_remove_request"||name=="effect_remove_checked")
                    o<<",\"effect\":"<<effectJson(int32(f.a[0]));
                else if(name=="effect_service")o<<",\"effect\":"<<effectJson(titleEffectSlot);
                else if(name=="mode_parent_tick"){const uint32 p=uint32(f.a[0]);
                    o<<",\"phase\":"<<int32(get32(p+0x20))<<",\"child\":"<<hx(get32(p+0x38))<<",\"request\":"<<int32(get32(p+0x3c))
                     <<",\"current\":"<<int32(get32(p+0x40));}
                else if(name=="mode_child_create"||name=="node_create")o<<",\"node\":"<<hx(uint32(v0));
                else if(name=="theme_retire_request")o<<",\"theme_words\":["<<int32(get32(0x2d9a60))<<","<<int32(get32(0x2d9a64))<<","<<int32(get32(0x2d9a68))<<"]";
                else if(name=="model_table_retire")o<<",\"material_list_head\":"<<hx(get32(materialListHead));
                else if(name=="logo_tick")o<<",\"tick\":"<<f.aux<<",\"counter_after\":"<<int32(get32(uint32(f.a[0])+0x30));
                else if(name=="storage_probe"||name=="storage_init"){
                    o<<",\"result\":"<<int64(v0)<<",\"param_words\":["<<int32(get32(f.aux2))<<","<<int32(get32(f.aux2+4))<<","<<int32(get32(f.aux2+8))<<"]"<<storageJson();
                }
                else if(name=="image_list_load"||name=="image_list_append"||name=="image_load_append"||name=="image_list_retire"
                        ||name=="image_slots_retire"||name=="resource_retire")
                    o<<",\"result\":"<<int32(v0)<<",\"image_list\":"<<intList(imageList());
                else if(name=="xmd_load_path"||name=="xmd_load"||name=="xmd_bind"||name=="xmd_model_build")
                    o<<",\"result\":"<<int32(v0)<<queueContext()<<",\"material_list_head\":"<<hx(get32(materialListHead));
                else if(name=="material_table")o<<",\"table\":"<<materialTable(f.aux2)<<",\"return_ptr\":"<<hx(v0);
                else if(name=="handle_successor")o<<",\"successor\":"<<int32(v0);
                else if(name=="upload_qwc"){
                    o<<",\"qwc\":"<<int32(v0);
                    // When called from 0x1c9bb8 (upload), hash exactly the qwc*16 bytes the upload DMA sends.
                    if(!stack.empty()&&std::string(stack.back().name)=="upload"){
                        const uint32 src=uint32(norm(stack.back().a[5]));const uint64 bytes=uint64(uint32(v0))*16;
                        o<<",\"upload_src\":"<<hx(src)<<",\"upload_bytes\":"<<bytes<<",\"upload_desc\":"<<descJson(stack.back().a[0]);
                        if(src&&plausiblePtr(src,bytes))o<<",\"upload_sha256\":\""<<sha(ram+src,bytes)<<"\"";else o<<",\"upload_sha256\":null";
                    }
                }
                else if(name=="range_alloc"||name=="reserve_class_set"||name=="cache_enable_set"||name=="lower_enable_set")o<<",\"result\":"<<int32(v0);
                else if(name=="handle_reserve")o<<",\"first\":"<<int32(v0)<<queueContext();
                else if(name=="handle_create"||name=="handle_alias"||name=="handle_lookup"){
                    const int h=int32(v0);o<<",\"record\":"<<handleRecord(h)<<queueContext();
                }
                else if(name=="xim_load"||name=="image_alloc_handle_upload")o<<",\"record\":"<<handleRecord(int32(v0));
                else if(name=="handle_retire")o<<",\"record_after\":"<<handleRecord(int32(f.a[0]));
                else if(name=="retain")o<<",\"refs_after\":"<<descRefs(f.a[0]);
                else if(name=="free"){
                    const uint32 av=get32(0x3adc1c);
                    o<<",\"available_after\":"<<av<<",\"freed\":"<<(av==f.aux2+1?"true":"false");
                    if(av!=f.aux2&&av!=f.aux2+1)throw std::runtime_error("0x1cf728 changed the available count by more than one");
                }
                else if(name=="palette_bind"||name=="palette_bind_desc")o<<",\"refs_after\":"<<descRefs(f.a[name=="palette_bind"?2:1]);
                else if(name=="file_load")o<<",\"buf\":"<<hx(v0)<<",\"last_raw_read\":{\"name\":\""<<esc(lastFile.name)<<"\",\"size\":"<<lastFile.size<<",\"sha256\":"
                                              <<(lastFile.sha.empty()?std::string("null"):"\""+lastFile.sha+"\"")<<"}";
            }
            if(restoreCancel&&successorSeen)restoreExitDecode(name,f,o);
            emit(o.str());
            if(retire&&name=="mode8_construct"&&int32(get32(uint32(f.a[0])+0x24))==1){
                if(!successorSeen)checkpoint(cpRetire("title-constructed")); // the Title stop point (paused at the 0x12c350 return address); continue
                else {
                    // Restore stop: the RETURN of the second 0x12c350(selector 1), before its first 0x1296f8.
                    if(padStep!=8)throw std::runtime_error("second title constructed without the Restore Cancel");
                    if(!minimal&&!restoreStopArmed)throw std::runtime_error("second title constructed before the Restore wrapper free");
                    checkpoint("second-title-return");
                }
            }
            if(restoreCancel&&successorSeen){
                if(name=="successor_construct")checkpoint("restore-constructor-return");
                else if(name=="restore_input")restoreScriptExit(f.aux);
                else if(name=="resource_tick"&&!taken("first-layout")&&taken("restore-constructor-return")&&uiPlaceCalls>f.aux2){
                    const uint32 par=get32(uint32(f.a[0])+0x14);
                    if(nodeKind(par)!="mode9_10"||int32(get32(par+0x24))!=10)throw std::runtime_error("first UI layout outside the mode-10 wrapper");
                    checkpoint("first-layout");
                }
            }
            if(stopHere){
                titleStopped=true;titleStopRa=f.ra;titleStopSp=f.sp;
                // The return breakpoint stays armed: the VM is paused AT the return address, before 0x1296f8.
                return;
            }
            if(f.dynamicBp)delBp(f.ra);
        };

        auto fillGs=[&]{
            // Called before the first guest instruction: PC must be the retail entry point.
            if(uint32(cpu.nPC)!=entryPc)throw std::runtime_error("EE is not at the retail entry point before the first resume");
            gs->SendGSCall([]{},true);
            const uint8* vram=gs->GetRam();bool zero=true;for(uint32 i=0;i<CGSHandler::RAMSIZE;++i)if(vram[i]){zero=false;break;}
            uint64 regs,kicks,tr,res;
            {std::lock_guard lock(gs->mutex);regs=gs->regWrites;kicks=gs->kicks;tr=gs->transfers;res=gs->resets;}
            gs->SendGSCall([&]{
                std::memset(gs->GetRam(),fill,CGSHandler::RAMSIZE);gs->armed=true;
                if(title&&!minimal){
                    std::lock_guard lock(gs->mutex);
                    gs->title=true;gs->eeEventCounter=&evAtomic;
                    gs->writer.assign(CGSHandler::RAMSIZE,0);gs->uploadEver.assign(CGSHandler::RAMSIZE,0);
                }
            },true);
            std::ostringstream o;
            o<<"{\"fill\":"<<unsigned(fill)<<",\"ee_pc\":"<<hx(cpu.nPC)<<",\"vram_all_zero_before_fill\":"<<(zero?"true":"false")
             <<",\"gs_reg_writes_before_fill\":"<<regs<<",\"gs_vertex_kicks_before_fill\":"<<kicks
             <<",\"gs_transfers_before_fill\":"<<tr<<",\"gs_resets_before_fill\":"<<res<<"}";
            fillReport=o.str();
        };
        auto isControlTransfer=[](uint32 w){
            const uint32 op=w>>26,fn=w&0x3f,rs=(w>>21)&0x1f;
            if(op==2||op==3||(op>=4&&op<=7)||op==1||(op>=0x14&&op<=0x17))return true;
            if(op==0&&(fn==8||fn==9))return true;
            if((op==0x10||op==0x11||op==0x12)&&rs==8)return true;
            return false;
        };
        auto firstControlTransfer=[&](uint32 pc)->uint32{
            if(pc<0x100000||pc>=0x1f8434)return 0;
            for(uint32 a=pc;a<0x1f8434&&a<pc+0x2000;a+=4)if(isControlTransfer(elfWord(a)))return a;
            return 0;
        };

        // Allocator/handle state snapshot (reads only). vram == nullptr: no VRAM digest (prefix snapshot).
        auto buildState=[&](const uint8* vram)->std::string{
        auto s16at=[&](uint32 a){int16 v;std::memcpy(&v,ram+(a&0x1ffffff),2);return int(v);};
        auto u64at=[&](uint32 a){uint64 v;std::memcpy(&v,ram+(a&0x1ffffff),8);return v;};
        auto require=[](bool ok,const char* why){if(!ok)throw std::runtime_error(why);};
        auto allocId=[](uint64 d){return d?int((d>>20)&0xfff):-1;};
        std::ostringstream out;
        {
            const uint32 R=get32(0x3a5af0),X=get32(0x3a5af4),rec=get32(0x3a7b00),extraDesc=get32(0x3a7b04),extraNext=get32(0x3a7b08),hashT=get32(0x3a7b0c);
            const int hsize=int(get32(0x2138c0));
            out<<"{\"kind\":\"final_state\",\"pc\":"<<hx(cpu.nPC)<<",\"handles\":[";
            bool first=true;
            for(int id=0;id<0x1000;++id){const int r=s16at(0x3a5b00+id*2);if(r>=0){out<<(first?"":",")<<'['<<id<<','<<r<<']';first=false;}}
            out<<"],\"records\":[";
            std::vector<int> live;
            if(R){int e=s16at(R+2);for(int n=0;e!=1&&n<=0x208;++n){require(e>=8&&e<0x208,"record list node");live.push_back(e-8);e=s16at(R+e*4+2);}}
            std::sort(live.begin(),live.end());first=true;
            for(int r:live){const uint32 b=rec+r*0x30;const std::string name=cstr(b+0x10);
                out<<(first?"":",")<<"{\"r\":"<<r<<",\"image\":"<<allocId(u64at(b))<<",\"pal0\":"<<allocId(u64at(b+8))
                   <<",\"name\":\""<<esc(name)<<"\",\"hash_slot\":"<<s16at(b+0x24)<<",\"handles\":"<<(get32(b+0x2c)&0xffff)<<",\"extra\":[";
                first=false;bool f2=true;int e=s16at(b+0x2e);
                for(int n=0;e!=-1;++n){require(n<=0x80&&e>=0&&e<0x80,"extra chain");
                    out<<(f2?"":",")<<allocId(u64at(extraDesc+e*8));f2=false;e=s16at(extraNext+e*2);}
                out<<"]}";}
            out<<"],\"hash\":[";first=true;
            for(int i=0;hashT&&i<hsize;++i){const int v=s16at(hashT+i*2);if(v){out<<(first?"":",")<<'['<<i<<','<<v<<']';first=false;}}
            out<<"],\"live_handles\":"<<int(get32(0x3a5af8))<<",\"reserved\":[";
            {first=true;int e=s16at(0x3a1ad0+2*4+2);for(int n=0;e!=4;++n){require(n<=0x1008&&e>=8&&e<0x1008,"reserved queue node");
                out<<(first?"":",")<<(e-8);first=false;e=s16at(0x3a1ad0+e*4+2);}}
            auto freeCount=[&](uint32 pool,int limit){int c=0;if(!pool)return 0;int e=s16at(pool+6*4+2);
                while(e!=7){require(c<=limit&&e>=8&&e<limit+8,"free list node");++c;e=s16at(pool+e*4+2);}return c;};
            out<<"],\"free_records\":"<<freeCount(R,0x200)<<",\"free_extras\":"<<freeCount(X,0x80)<<",\"descriptors\":[";
            std::vector<std::pair<int,int>> d;
            {int e=s16at(0x3adc20+0x1000*6+4);
             for(int n=0;e!=0x1001;++n){require(n<=0x1000&&e>=0&&e<0x1000,"descriptor ring node");d.push_back({e,s16at(0x3adc20+e*6)});e=s16at(0x3adc20+e*6+4);}
             std::sort(d.begin(),d.end());first=true;
             for(auto& p:d){out<<(first?"":",")<<'['<<p.first<<','<<p.second<<']';first=false;}}
            out<<"],\"allocations\":[";first=true;
            for(const auto& kv:allocs){
                int refs=-1;for(auto& p:d)if(uint64(p.first)==kv.first)refs=p.second;
                const auto& a=kv.second;
                out<<(first?"":",")<<"{\"id\":"<<kv.first<<",\"class\":"<<a.cls<<",\"req_psm\":"<<a.psm<<",\"req_w\":"<<a.w<<",\"req_h\":"<<a.h
                   <<",\"desc\":"<<descJson(a.desc)<<",\"live\":"<<(refs>=0?"true":"false")<<",\"refs\":"<<refs<<"}";first=false;}
            out<<"],\"vram\":{\"available\":"<<get32(0x3adc1c)<<",\"cursors\":[";
            for(unsigned j=0;j<64;++j)out<<(j?",":"")<<get32(0x3adb18+j*4);
            out<<"],\"occupancy_sha256\":\""<<sha(ram+0x3abb18,8192)<<"\",\"nodes_sha256\":\""<<sha(ram+0x3adc20,4096*6+24)
               <<"\",\"run_fill\":"<<unsigned(fill)<<",\"run_vram_sha256\":"<<(vram?"\""+sha(vram,CGSHandler::RAMSIZE)+"\"":std::string("null"))<<"},"
               <<randState()<<",\"free_count_0x3adc1c\":"<<get32(0x3adc1c)<<",\"ee_ram_32mb_sha256\":\""<<sha(ram,0x2000000)
               <<"\",\"ee_code_sha256\":\""<<sha(ram+0x100000,0x1f8434-0x100000)<<"\"";
            if(title) {
                // Title: complete native-comparable allocator/handle state, read from guest RAM.
                auto ring=[&](int head,int tail){std::vector<int> v;int e=s16at(0x3adc20+head*6+4);
                    for(int n=0;e!=tail;++n){require(n<=0x1000&&e>=0&&e<0x1000,"descriptor ring node");v.push_back(e);e=s16at(0x3adc20+e*6+4);}return v;};
                auto poolOrder=[&](uint32 pool,int head,int tail,int limit){std::vector<int> v;if(!pool)return v;int e=s16at(pool+head*4+2);
                    for(int n=0;e!=tail;++n){require(n<=limit&&e>=8&&e<limit+8,"pool list node");v.push_back(e-8);e=s16at(pool+e*4+2);}return v;};
                out<<",\"allocation_serials\":[";first=true;
                for(const auto& kv:allocs){out<<(first?"":",")<<'['<<kv.first<<','<<kv.second.serial<<']';first=false;}
                out<<"],\"free_queue_ids\":"<<intList(ring(0x1002,0x1003))<<",\"inuse_ring_ids\":"<<intList(ring(0x1000,0x1001))
                   <<",\"reserved1\":"<<intList(queueIds(3,5))<<",\"reserve_class\":"<<int32(get32(reserveClass))
                   <<",\"cache_switch\":"<<int32(get32(cacheSwitch))<<",\"lower_switch\":"<<int32(get32(lowerSwitch))
                   <<",\"image_list_count\":"<<int32(get32(imageListCount))<<",\"image_list\":"<<intList(imageList())
                   <<",\"record_free_order\":"<<intList(poolOrder(R,6,7,0x200))<<",\"extra_free_order\":"<<intList(poolOrder(X,6,7,0x80))
                   <<",\"handle_ids_free_order\":"<<intList(queueIds(6,7))
                   <<",\"handle_range_sha256\":\""<<sha(ram+0x3a7b10,0x1001*4)<<"\",\"handle_nodes_sha256\":\""<<sha(ram+0x3a1ad0,0x1008*4)<<"\""
                   <<",\"material_tables\":[";
                {uint32 t=get32(materialListHead);for(int n=0;t&&n<64;++n){out<<(n?",":"")<<materialTable(t);if(!plausiblePtr(t,4))break;t=get32(t);}}
                static const char* hexd="0123456789abcdef";
                out<<"],\"occupancy_hex\":\"";
                for(uint32 k=0;k<8192;++k){const uint8 b=ram[0x3abb18+k];out<<hexd[b>>4]<<hexd[b&15];}
                out<<"\"";
            }
            if(retire) {
                // Retire: effect slot table (live jobs) and the tracked title water effect.
                out<<",\"effect_slots\":[";first=true;
                for(int32 s=0;s<int32(effectSlots);++s){const uint32 j=effectJob(s);if(j==0xffffffffu)continue;
                    out<<(first?"":",")<<effectJson(s);first=false;}
                out<<"],\"title_effect_slot\":"<<titleEffectSlot<<",\"title_effect\":"<<effectJson(titleEffectSlot)
                   <<",\"theme_words_2d9a60\":["<<int32(get32(0x2d9a60))<<","<<int32(get32(0x2d9a64))<<","<<int32(get32(0x2d9a68))<<"]";
            }
            out<<"}\n";
        }
        return out.str();
        };
        checkpoint=[&](const std::string& cp){
            if(std::find(checkpointsTaken.begin(),checkpointsTaken.end(),cp)!=checkpointsTaken.end())throw std::runtime_error("checkpoint repeated: "+cp);
            checkpointsTaken.push_back(cp);
            const auto dir=outDir/"checkpoints"/cp;
            if(std::filesystem::exists(dir))throw std::runtime_error("checkpoint directory exists");
            std::filesystem::create_directories(dir);
            // Submit already-processed GS register writes and wait for the GS thread. No write-buffer rotation and no
            // ledger transfer split (an open transfer is reported, not flushed).
            gs->SubmitWriteBuffer();
            gs->SendGSCall([]{},true);
            const uint8* vram=gs->GetRam();
            writeFresh(dir/"vram.bin",vram,CGSHandler::RAMSIZE);
            std::string transfer="null";
            {
                std::lock_guard lock(gs->mutex);
                if(!minimal){
                    writeFresh(dir/"vram-last-writer.bin",gs->writer.data(),gs->writer.size());
                    writeFresh(dir/"vram-upload-ever.bin",gs->uploadEver.data(),gs->uploadEver.size());
                }
                std::ostringstream t;
                t<<"{\"open\":"<<(gs->current.open?"true":"false")<<",\"seq\":"<<gs->current.seq<<",\"expected\":"<<gs->current.total
                 <<",\"received\":"<<gs->current.received<<",\"transfers\":"<<gs->transfers<<",\"flips\":"<<gs->flips<<",\"gs_ops\":"<<gs->gsOps<<"}";
                transfer=t.str();
            }
            const std::string s=buildState(vram);
            writeFresh(dir/"state.jsonl",s.data(),s.size());
            emit("\"ev\":\"checkpoint\",\"name\":\""+cp+"\",\"pc\":"+hx(cpu.nPC)+",\"title_n\":"+std::to_string(titleUpdates)
                 +",\"stack_depth\":"+std::to_string(stack.size())+",\"gs\":"+transfer+","+randState());
            if(restoreCancel&&(successorSeen||cp=="retire-successor-entry"))
                emit("\"ev\":\"glyph_state\",\"checkpoint\":\""+cp+"\","+glyphStateJson()+","+themeJson());
        };
        fillGs();
        const auto startTime=std::chrono::steady_clock::now();
        vm.Resume();
        const auto deadline=startTime+std::chrono::seconds(deadlineSeconds);
        uint64 stops=0,benignBlockStops=0,randCalls=0;std::vector<uint32> history;
        bool randPending=false;uint32 randPendingRa=0;uint64 randPendingSp=0;
        while(!stopped&&!exited&&std::chrono::steady_clock::now()<deadline) {
            if(stateChanged.exchange(false)&&vm.GetStatus()==CVirtualMachine::PAUSED) {
                const uint32 pc=cpu.nPC;++stops;if(history.size()<100000)history.push_back(pc);
                const uint64 sp=reg(CMIPS::SP);
                bool explained=false;
                // RNG: rand() return (leaf; one pending call at most) before everything else at this pc.
                if(randPending&&pc==randPendingRa&&sp==randPendingSp) {
                    emit("\"ev\":\"rand_exit\",\"n\":"+std::to_string(randCalls-1)+",\"ra\":"+hx(pc)+",\"v0\":"+std::to_string(uint32(reg(CMIPS::V0)))
                         +",\"v0_hi\":"+std::to_string(uint32(reg(CMIPS::V0)>>32))+",\"state\":"+std::to_string(get32(0x214ec8+0x58))
                         +",\"rand_ptr_2151b4\":"+std::to_string(get32(0x2151b4)));
                    randPending=false;delBp(randPendingRa);explained=true;
                }
                // exits first (return addresses), then entries, then top-level call sites
                for(;;) {
                    bool any=false;
                    for(size_t i=stack.size();i-->0;)
                        if(stack[i].ra==pc&&stack[i].sp==sp){onExitAt(i);any=true;explained=true;break;}
                    if(!any||titleStopped)break;
                }
                if(titleStopped) {
                    // Title stop: paused AT the return address of 0x12c350(selector 1); nothing after the
                    // constructor's return has executed (in particular no 0x1296f8 title update).
                    emit("\"ev\":\"stop\",\"kind\":\"title_constructor_return\",\"pc\":"+hx(pc)+",\"sp\":"+hx(sp)
                         +",\"stack_depth\":"+std::to_string(stack.size())+","+randState());
                    stopped=true;break;
                }
                if(pc==randPc){
                    if(!rngProducts) emit("\"ev\":\"rand\",\"ra\":"+hx(reg(CMIPS::RA))+",\"state\":"+std::to_string(get32(0x214ec8+0x58)));
                    else {
                        if(randPending)throw std::runtime_error("Nested rand() call");
                        std::string frames="[";
                        for(size_t k=0;k<stack.size();++k)frames+=std::string(k?",":"")+"\""+stack[k].name+"\"";
                        frames+="]";
                        emit("\"ev\":\"rand\",\"n\":"+std::to_string(randCalls)+",\"ra\":"+hx(reg(CMIPS::RA))+",\"sp\":"+hx(sp)
                             +",\"state\":"+std::to_string(get32(0x214ec8+0x58))+",\"rand_ptr_2151b4\":"+std::to_string(get32(0x2151b4))
                             +",\"parent\":"+(stack.empty()?std::string("null"):"\""+std::string(stack.back().name)+"\"")+",\"frames\":"+frames);
                        randPending=true;randPendingRa=uint32(reg(CMIPS::RA));randPendingSp=sp;addBp(randPendingRa);
                    }
                    ++randCalls;explained=true;
                }
                if(entryName.count(pc)){onEnter(pc);explained=true;}
                if(retire&&!minimal){
                    for(const auto& pr:titleRetireProbeDefs)if(pr.addr==pc){
                        // Retire probe at an exact jalr+8 block start: indirect-call target sources and job words.
                        std::ostringstream o;o<<"\"ev\":\"probe\",\"pc\":"<<hx(pc)<<",\"name\":\""<<pr.name<<"\"";
                        // 0x102218: s6 = slot index (0x1024d4 passes it to 0x1131d0); 0x102cf8: slot index at sp+0xc0 (0x102e20).
                        if(pc==0x1024d0){const int32 sl=int32(reg(CMIPS::S6));const uint32 j=effectJob(sl);
                            o<<",\"slot\":"<<sl<<",\"job\":"<<hx(j)<<",\"target_job_68\":"<<(plausiblePtr(j,0x80)?hx(get32(j+0x68)):std::string("null"));}
                        else if(pc==0x102e4c){const int32 sl=int32(get32(uint32(sp)+0xc0));const uint32 j=effectJob(sl);
                            o<<",\"slot\":"<<sl<<",\"job\":"<<hx(j);
                            if(plausiblePtr(j,0x80))o<<",\"target_job_68\":"<<hx(get32(j+0x68))<<",\"rgba_40\":["<<hx(get32(j+0x40))<<","<<hx(get32(j+0x44))
                                                  <<","<<hx(get32(j+0x48))<<","<<hx(get32(j+0x4c))<<"],\"flags\":"<<hx(get32(j+4));}
                        else if(pc==0x103ba8)o<<",\"target_213574\":"<<hx(get32(0x213574))<<",\"size_s0\":"<<int32(reg(CMIPS::S0))<<",\"v0\":"<<hx(uint32(reg(CMIPS::V0)));
                        else if(pc==0x103c28)o<<",\"target_213580\":"<<hx(get32(0x213580))<<",\"ptr_s0\":"<<hx(uint32(reg(CMIPS::S0)));
                        else if(pc==0x103c70)o<<",\"target_21357c\":"<<hx(get32(0x21357c))<<",\"ptr_s1\":"<<hx(uint32(reg(CMIPS::S1)))<<",\"arg_s0\":"<<hx(uint32(reg(CMIPS::S0)));
                        emit(o.str());explained=true;
                    }
                }
                if(restoreArmed){
                    for(const auto& pr:restoreProbeDefs)if(pr.addr==pc){
                        std::ostringstream o;o<<"\"ev\":\"probe\",\"pc\":"<<hx(pc)<<",\"name\":\""<<pr.name<<"\"";
                        if(pc==0x1984b0)o<<",\"window\":"<<windowFields(uint32(reg(CMIPS::S0)));
                        else if(pc>=0x1aee00)o<<",\"hook_202ddc\":"<<hx(get32(0x202ddc))<<",\"w202dd8\":"<<hx(get32(0x202dd8))<<",\"v0\":"<<hx(uint32(reg(CMIPS::V0)));
                        emit(o.str());explained=true;
                    }
                }
                if(retireStopped) {
                    emit("\"ev\":\"stop\",\"kind\":\"successor_constructor_entry\",\"pc\":"+hx(pc)+",\"sp\":"+hx(sp)
                         +",\"route\":\""+route+"\",\"mode\":"+std::to_string(routeMode)+",\"stack_depth\":"+std::to_string(stack.size())+","+randState());
                    titleStopSp=sp;stopped=true;break;
                }
                const uint32 ct=firstControlTransfer(pc);
                auto top=ct?topSites.find(ct):topSites.end();
                if(top!=topSites.end()) {
                    topStep.store(int(ct));explained=true;
                    if(title&&ct==stopPc&&!prefixPassed) {
                        // First arrival at the Startup/RNG boundary: record it (and a state snapshot) and continue.
                        prefixPassed=true;
                        emit("\"ev\":\"prefix_end\",\"block_start\":"+hx(pc)+",\"pc\":"+hx(ct)+",\"callee\":"+hx(top->second)+",\"stack_depth\":"+std::to_string(stack.size())+","+randState());
                        const std::string s=buildState(nullptr);
                        writeFresh(outDir/"state-at-100618.jsonl",s.data(),s.size());
                    }
                    else if(!title&&ct==stopPc) {
                        emit("\"ev\":\"stop\",\"block_start\":"+hx(pc)+",\"pc\":"+hx(ct)+",\"callee\":"+hx(top->second)+",\"stack_depth\":"+std::to_string(stack.size())+","+randState());
                        stopped=true;break; // stay paused: the jal at 0x100618 has NOT executed
                    }
                    emit("\"ev\":\"top\",\"block_start\":"+hx(pc)+",\"pc\":"+hx(ct)+",\"callee\":"+hx(top->second));
                }
                if(!explained) {
                    // A block start that contains some breakpoint address but no event we model.
                    bool contains=false;
                    if(ct)for(const auto& kv:bpRef)if(kv.second>0&&kv.first>=pc&&kv.first<=ct+4){contains=true;break;}
                    if(!contains) {
                        std::ostringstream d;d<<"Unexplained stop at EE PC "<<hx(pc)<<" ct="<<hx(ct)<<" ra="<<hx(reg(CMIPS::RA))
                            <<" stops="<<stops<<" stack="<<stack.size()<<" exc="<<cpu.nHasException;
                        throw std::runtime_error(d.str());
                    }
                    ++benignBlockStops;
                }
                vm.Resume();continue;
            }
            std::unique_lock lock(stateMutex);
            stateWake.wait_for(lock,std::chrono::milliseconds(50),[&]{return stateChanged.load()||exited.load();});
        }
        if(!stopped)throw std::runtime_error("Startup did not reach the stop PC (exit="+std::to_string(exited.load())+" stops="+std::to_string(stops)+")");
        if(vm.GetStatus()!=CVirtualMachine::PAUSED)throw std::runtime_error("VM not paused at stop");
        const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-startTime).count();
        if(uint32(cpu.nPC)!=(restoreCancel?titleStopRa:retire?0x124548u:title?titleStopRa:stopPc))throw std::runtime_error("Stop PC mismatch");
        if(restoreCancel){
            static const char* need[]={"retire-successor-entry","restore-constructor-return","first-layout","empty-ready","cancel-death",
                                       "resource-retirement","wrapper-free","second-title-return"};
            for(const char* n:need){
                const std::string s=n;
                if(minimal&&s!="retire-successor-entry"&&s!="empty-ready"&&s!="second-title-return")continue; // not hooked in the control
                if(!taken(s))missingCheckpoints.push_back(s); // reported in run-meta; exit status 1
            }
        }
        if(title&&!prefixPassed)throw std::runtime_error("Title stop reached without passing 0x100618");

        // ---- final state (VM paused, GS drained) -----------------------------------------
        // Finish: flush any buffered GS register writes, then wait for the GS thread.
        std::string dmaReport;
        {
            std::ostringstream o;
            o<<"{\"dma2_chcr\":"<<vm.m_ee->m_dmac.GetRegister(CDMAC::D2_CHCR)<<",\"dma1_chcr\":"<<vm.m_ee->m_dmac.GetRegister(CDMAC::D1_CHCR)
             <<",\"gif_active_path\":"<<vm.m_ee->m_gif.GetActivePath()<<"}";dmaReport=o.str();
        }
        gs->FlushWriteBuffer();
        gs->SendGSCall([]{},true);
        {std::lock_guard lock(gs->mutex);gs->flushTransfer();}
        const uint8* vram=gs->GetRam();
        writeFresh(outDir/"vram.bin",vram,CGSHandler::RAMSIZE);
        writeFresh(outDir/"ee-ram-32mb.bin",ram,0x2000000);
        if(title&&!minimal){
            std::lock_guard lock(gs->mutex);
            writeFresh(outDir/"vram-last-writer.bin",gs->writer.data(),gs->writer.size());
            writeFresh(outDir/"vram-upload-ever.bin",gs->uploadEver.data(),gs->uploadEver.size());
        }
        const std::string out=buildState(vram);
        writeFresh(outDir/"final-state.jsonl",out.data(),out.size());

        // GS ledger
        {
            std::ostringstream o;
            {std::lock_guard lock(gs->mutex);
             for(const auto& l:gs->lines)o<<l<<'\n';
             if(title)gs->writeTitleSummary(o);
             o<<"{\"kind\":\"gs_summary\",\"register_writes\":"<<gs->regWrites<<",\"vertex_kicks\":"<<gs->kicks<<",\"host_to_local_transfers\":"<<gs->transfers
              <<",\"host_to_local_completed\":"<<gs->hostToLocalDone<<",\"local_to_host\":"<<gs->localToHost<<",\"local_to_local\":"<<gs->localToLocal
              <<",\"clut_transfers\":"<<gs->clutTransfers<<",\"resets\":"<<gs->resets<<",\"register_histogram\":{";
             bool first=true;for(const auto& kv:gs->regHistogram){o<<(first?"":",")<<'"'<<unsigned(kv.first)<<"\":"<<kv.second;first=false;}
             o<<"},\"dma_gif_at_stop\":"<<dmaReport<<"}\n";}
            writeFresh(outDir/"gs-ledger.jsonl",o.str().data(),o.str().size());
        }
        // Disc opens
        {
            std::ostringstream o;{std::lock_guard lock(traced->mutex);for(const auto& r:traced->rows)o<<r<<'\n';}
            writeFresh(outDir/"disc-opens.jsonl",o.str().data(),o.str().size());
        }
        if(retire){
            std::ostringstream o;{std::lock_guard lock(scriptedPad->mutex);for(const auto& r:scriptedPad->rows)o<<r<<'\n';}
            writeFresh(outDir/"pad-host.jsonl",o.str().data(),o.str().size());
        }
        events.flush();events.close();
        {
            std::ostringstream o;
            o<<"{\"fill\":"<<unsigned(fill)<<",\"fill_report\":"<<fillReport<<",\"events\":"<<evCount<<",\"breakpoint_stops\":"<<stops<<",\"benign_block_start_stops\":"<<benignBlockStops<<",\"rand_calls\":"<<randCalls<<",\"minimal\":"<<(minimal?"true":"false")<<",\"rng_products\":"<<(rngProducts?"true":"false")<<",\"rand_pending_at_stop\":"<<(randPending?"true":"false")
             <<",\"vblanks\":"<<frames.load()<<",\"wall_seconds\":"<<std::fixed<<std::setprecision(2)<<seconds
             <<",\"open_frames_at_stop\":[";
            for(size_t i=0;i<stack.size();++i)o<<(i?",":"")<<"\""<<stack[i].name<<"\"";
            o<<"],\"stack_depth_at_stop\":"<<stack.size()<<",\"static_return_sites\":"<<returnSites.size();
            if(title)o<<",\"title\":true,\"stop_pc\":"<<hx(cpu.nPC)<<",\"stop_sp\":"<<hx(titleStopSp)<<",\"prefix_passed\":"<<(prefixPassed?"true":"false");
            if(retire){
                o<<",\"title_retire\":\""<<route<<"\",\"route_mode\":"<<routeMode<<",\"title_updates\":"<<titleUpdates<<",\"pad_step\":"<<padStep
                 <<",\"title_effect_slot\":"<<titleEffectSlot<<",\"checkpoints\":[";
                for(size_t i=0;i<checkpointsTaken.size();++i)o<<(i?",":"")<<"\""<<checkpointsTaken[i]<<"\"";
                o<<"]";
            }
            if(restoreCancel){
                o<<",\"restore_cancel\":true,\"memcard_mc0\":\""<<esc(card0.string())<<"\",\"memcard_mc1\":\""<<esc(card1.string())
                 <<"\",\"restore_hooks_armed\":"<<(restoreArmed?"true":"false")<<",\"restore_hooks\":"<<restoreEntryName.size()
                 <<",\"restore_return_sites\":"<<restoreReturnSites.size()<<",\"restore_input_calls\":"<<restoreInputCalls
                 <<",\"ui_place_calls\":"<<uiPlaceCalls<<",\"missing_checkpoints\":[";
                for(size_t i=0;i<missingCheckpoints.size();++i)o<<(i?",":"")<<"\""<<missingCheckpoints[i]<<"\"";
                o<<"]";
            }
            o<<"}\n";
            writeFresh(outDir/"run-meta.json",o.str().data(),o.str().size());
        }
        if(restoreCancel){
            const std::string s="{\"mc0\":"+cardListing(card0)+",\"mc1\":"+cardListing(card1)+"}\n";
            writeFresh(outDir/"memcards-after.json",s.data(),s.size());
        }
        finishing=true;
        std::cout<<"OK events="<<evCount<<" stops="<<stops<<" vblanks="<<frames.load()<<" seconds="<<seconds<<std::endl;
        if(!missingCheckpoints.empty()){std::cerr<<"Restore checkpoints not reached: "<<missingCheckpoints.size()<<'\n';return 1;}
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
