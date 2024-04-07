#include "okage/name_entry.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace okage {
NameEntryInterface::NameEntryInterface(BattleUiShared& shared,NameEntryShared& name,
    const NameEntryData& data,std::shared_ptr<RuwResource> resource,
    std::shared_ptr<const void> texture,std::uint32_t flags)
    :shared_(shared),name_(name),data_(data),resource_(std::move(resource)) {
    if(!resource_||!texture)throw std::runtime_error("Name entry requires owned UI resources");
    shared_.atlas_texture=-1;
    const auto& d=data_.definition;const auto& row=d.words;
    const auto free=std::find(shared_.registry.begin(),shared_.registry.end(),nullptr);
    if(free==shared_.registry.end())throw std::runtime_error("Name entry window registry exhausted");
    const unsigned slot=free-shared_.registry.begin();
    try {
        *free=std::make_shared<UiWindow>(d.glyph_capacity(),d.sprite_capacity(),slot,name_.stream);
        handle_=int(slot);
        if((*free)->configure(row,data_.atlas,texture))shared_.resource.layout_shared().sprite_dirty=true;
        words_[0x1c/4]=flags&0x18000000u;
        constexpr unsigned base=0x40/4;
        words_[base]=slot;words_[base+1]=1;
        words_[base+0x40/4]=row[1];words_[base+0x44/4]=row[2];
        std::copy_n(row.begin()+4,12,words_.begin()+base+4);
        words_[base+0x4c/4]=0xffffffff;words_[base+0x54/4]=1;
        for(unsigned i=0;i<8;++i){words_[base+0x6c/4+i]=0xffffffff;words_[base+0xac/4+i]=0xffffffff;}
        words_[base+0x64/4]=0xffffffff;
        //1196a0: selects the name stream, writes14/6, terminates and clears flag.
        name_.stream->entries={UiStreamCommand{14,UiStreamReference::MenuTransform1f9c00},
                               UiStreamCommand{6,UiStreamReference::MenuTransform1f9c2c}};
        name_.flag=0;shared_.current_stream=1;shared_.stream_cursor=4;
        words_[base+0x50/4]=0x50;words_[base+0x5c/4]=1;words_[base+0x60/4]=9;
        words_[0xe90/4]=0;words_[0xe40/4]=1;
        //11f5f8 wrapper writes these after the synchronous row constructor.
        words_[0x8c/4]=words_[0xed8/4]=words_[0x168/4]=words_[0x16c/4]=words_[0x88/4]=0xffffffff;
        words_[0xe44/4]=words_[0x180/4];
    }catch(...){destroy_windows();throw;}
}
NameEntryInterface::~NameEntryInterface(){destroy_windows();}
void NameEntryInterface::destroy_windows(){if(handle_>=0){shared_.registry[unsigned(handle_)].reset();handle_=-1;}}
unsigned NameEntryInterface::handle() const {
    if(handle_<0||!shared_.registry[unsigned(handle_)])throw std::runtime_error("Name entry window lifetime");
    return unsigned(handle_);
}
UiWindow& NameEntryInterface::window(){return *shared_.registry[handle()];}
void NameEntryInterface::bind(unsigned group) {
    shared_.resource_accumulators[0]=0;
    const std::array<std::size_t,1> groups{group};window().bind_ruw(resource_,groups);
}
void NameEntryInterface::first_update(BattleUiPad& pad,BattleUiPadInput input,
    std::span<std::uint8_t> variables,const std::function<void(unsigned)>& sound) {
    if(variables.size()<0x84d||!sound)throw std::runtime_error("Name entry game state services");
    words_[0xe8c/4]=0;
    if(!(words_[0x90/4]&0x40))return;
    words_[0xe48/4]=0;words_[0x114/4]=1; //typed self link
    const auto masks=pad.normalize(input);
    auto& phase=words_[0xe4c/4];
    const auto get=[&](unsigned at){return std::bit_cast<std::int32_t>(words_.at(at/4));};
    const auto put=[&](unsigned at,int v){words_.at(at/4)=std::bit_cast<std::uint32_t>(std::int32_t(v));};
    const auto default_name=[&] {
        if(get(0xa8)==0){std::copy(data_.default_name.begin(),data_.default_name.end(),variables.begin()+0x844);put(0xa8,3);}
    };
    const auto confirm=[&] {
        default_name();put(0xdc,0);put(0xe0,0);put(0xe4,0);put(0xe8,0);put(0xcc,0);put(0xd8,1);
        phase=0xffffffffu;bind(17);
    };
    const auto selection=[&] {
        const int x=get(0xdc),y=get(0xe0);
        put(0xcc,y==-1?~x:x+y*13);put(0xac,get(0xcc));sound(0);
    };
    switch(std::bit_cast<std::int32_t>(phase)) {
    case 0:
        window().reset_for_open();window().set_opacity_tween(8,1,0,128);bind(9);
        put(0xa8,0);++phase;put(0xd0,1);put(0xdc,0);put(0xe0,0);put(0xa8,3);break;
    case 1: {
        put(0xa4,-1);bool erase=false;
        if(masks.pressed&0x40) {
            const int selected=get(0xcc);
            if(selected==-1||selected==-3){confirm();break;}
            if(selected==-2)erase=true;
            else {
                const std::string_view glyph(name_.selected_character.data(),name_.selected_character[0]?1:0);
                if(glyph!=data_.space&&glyph!=data_.empty) {
                    const int length=get(0xa8);
                    if(length<0||length>7)throw std::runtime_error("Name length outside source field");
                    variables[0x844+length]=std::uint8_t(name_.selected_character[0]);variables[0x845+length]=0;variables[0x84b]=0;
                    put(0xa8,length+1);
                    if(get(0xa8)>=7){put(0xa8,7);put(0xcc,-3);}
                    put(0xa4,get(0xa8));sound(3);
                }
            }
        }
        erase|=get(0xa8)>=0&&(masks.pressed&0x20);
        if(erase) {
            const int n=get(0xa8)-1;put(0xa8,n);
            if(n<0)put(0xa8,0);
            else {
                if(n>7)throw std::runtime_error("Name erasure extent");
                if(n==0)variables[0x844]=' ';else variables[0x844+n]=0;
                //Source loop repeatedly writes this same byte, not consecutive bytes.
                if(n*2+1<16)variables[0x845+n]=0;
                variables[0x84a]=variables[0x84b]=variables[0x84c]=0;sound(4);
            }
        }
        if(masks.pressed&0x800){default_name();put(0xcc,-3);put(0xdc,2);put(0xe0,-1);}
        if(masks.repeat&0x1000) {
            put(0xe0,get(0xe0)-1);
            if(get(0xe0)==-1){put(0xe4,get(0xdc));put(0xdc,get(0xdc)>11?2:get(0xdc)>9?1:0);}
            else if(get(0xe0)==3&&(get(0xdc)==10||get(0xdc)==11)){put(0xdc,get(0xdc)+1);put(0xe4,get(0xdc));}
            else put(0xdc,get(0xe4));
            if(get(0xe0)<-1){put(0xe0,4);if(get(0xdc)>=12)put(0xdc,11);else if(get(0xdc)==11)put(0xdc,10);}
            selection();
        }
        if(masks.repeat&0x4000) {
            put(0xe0,get(0xe0)+1);
            if(get(0xe0)==0&&(get(0xe4)==10||get(0xe4)==11)){put(0xdc,get(0xe4)+1);put(0xe4,get(0xdc));}
            if(get(0xe0)==4){if(get(0xdc)==11)put(0xdc,10);else if(get(0xdc)==12)put(0xdc,11);put(0xe4,get(0xdc));}
            if(get(0xe0)>4)put(0xe0,-1);
            if(get(0xe0)==-1){put(0xe4,get(0xdc));put(0xdc,get(0xdc)<10?0:get(0xdc)<11?1:2);}
            else put(0xdc,get(0xe4));
            selection();
        }
        if(masks.repeat&0x8000) {
            put(0xdc,get(0xdc)-1);
            if(get(0xdc)<0)put(0xdc,get(0xe0)==-1?2:get(0xe0)==4?11:12);
            if(get(0xe0)!=-1)put(0xe4,get(0xdc));selection();
        }
        if(masks.repeat&0x2000) {
            put(0xdc,get(0xdc)+1);
            if(get(0xe0)==-1){if(get(0xdc)>2)put(0xdc,0);}
            else put(0xdc,get(0xdc)%(get(0xe0)==4?12:13));
            if(get(0xe0)!=-1)put(0xe4,get(0xdc));selection();
        }
        name_.flag=0;break;
    }
    case -1:
        if(masks.repeat&0x4000){put(0xd8,std::min(get(0xd8)+1,1));put(0xb8,get(0xd8));}
        if(masks.repeat&0x1000){put(0xd8,std::max(get(0xd8)-1,0));put(0xb8,get(0xd8));}
        if(masks.pressed&0x20){phase=1;bind(9);}
        if(masks.pressed&0x40){
            if(get(0xd8)!=0){phase=1;bind(9);}
            else{window().set_opacity_tween(-4,128,0,128);sound(3);phase=0xfffffffeu;}
        }
        break;
    case -2:if(window().opacity()==0)phase=0xfffffffdu;break;
    case -3:words_[0x1c/4]|=0x80000000u;break;
    default:break;
    }
    ++words_[0xe8c/4];
}
}
