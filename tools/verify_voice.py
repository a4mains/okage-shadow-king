#!/usr/bin/env python3
"""Native envelopes/interpolation/looped voices compared with pinned DSP kernels."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import wave
from build_voice_reference import build, HASHES
from build_spu_reference import REVISION
from verify_audio import opening_samples
from verify_event import write_exact

def sha(b):return hashlib.sha256(b).hexdigest()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,default=Path('build/native/okage-assets'))
    p.add_argument('--output',type=Path,default=Path('local/voice-validation'))
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=True);reference=build()
    def compare(name,cmd,arguments):
        original=args.output/(name+'.reference.bin');native=args.output/(name+'.native.bin')
        if not original.exists():subprocess.run([str(reference),cmd,*map(str,arguments),str(original)],check=True)
        subprocess.run([str(args.native),{'table':'voice-table','envelope':'voice-envelope','voice':'voice-sample'}[cmd],*map(str,arguments),str(native)],check=True)
        data=native.read_bytes()
        if data!=original.read_bytes():
            unit=12 if cmd=='envelope' else (2 if cmd=='table' else 4)
            first=next(i for i,(a,b) in enumerate(zip(data,original.read_bytes())) if a!=b)//unit
            raise RuntimeError(name+': mismatch at '+str(first))
        return {'name':name,'bytes':len(data),'sha256':sha(data),'exact':True}
    report={'scope':"Pinned PCSX2 DSP kernels with direct voice start and fixed gains; no whole-device, reverb or hardware fidelity claim",
            'reference_revision':REVISION,'reference_source_hashes':HASHES,'cases':[]}
    report['cases'].append(compare('gaussian-table','table',[]))
    bank=Path('local/audio-input/SNDDATA/BGM/EVS_MSH0.XV').read_bytes();_,body,tones=opening_samples(bank)
    metadata=subprocess.check_output([str(args.native),'music-bank','local/audio-input/SNDDATA/BGM/EVS_MSH0.XV'],text=True)
    rows=[list(map(int,line.split())) for line in metadata.splitlines()]
    for i,(tone,row) in enumerate(zip(tones,rows)):
        assert tone['offset']==row[3] and tone['program']==row[0]
        source=args.output/('tone-%02d.adpcm'%i);write_exact(source,body[tone['offset']:tone['offset']+tone['bytes']])
        report['cases'].append(compare('tone-%02d'%i,'voice',[source,row[4],row[5],3176,30,80000,100000]))
    # Cover zero/high pitch, one-shot end, signed gains, and loop history using bounded synthetic blocks.
    blocks=bytearray([0x10,4]+[0x77]*14+[0x20,3]+[0x87]*14)
    source=args.output/'loop-stress.adpcm';write_exact(source,blocks)
    for pitch,gain in [(0,127),(1,-64),(4096,128),(16383,30)]:
        report['cases'].append(compare('loop-%d'%pitch,'voice',[source,0x80ff,0x3f, pitch,gain,40000,60000]))
    blocks[17]=1;source=args.output/'one-shot.adpcm';write_exact(source,blocks)
    report['cases'].append(compare('one-shot','voice',[source,0xff,0x1f,4096,128,40000,60000]))
    for i,(a,b) in enumerate([(0,0),(0x80ff,0x5fcd),(0xffff,0xffff),(0x2c3a,0xa42d),(0x303f,0x502c),(0x83ff,0x1fc0),(0x7fff,0x9fc0),(0x81ff,0xdfe0)]):
        report['cases'].append(compare('envelope-%d'%i,'envelope',[a,b,60000,100000]))
    pcm=args.output/'opening-dry.pcm'
    log=subprocess.check_output([str(args.native),'music-dry','local/audio-input/SNDDATA/BGM/EVS_MSH.XSQ',
                                 'local/audio-input/SNDDATA/BGM/EVS_MSH0.XV','4288',str(pcm)],text=True)
    data=pcm.read_bytes();samples=struct.unpack('<%dh'%(len(data)//2),data)
    report['dry_music']={'frames':len(data)//4,'rate':48000,'sha256':sha(data),'peak':max(abs(x) for x in samples),
                         'clipped_samples':sum(x in [-32768,32767] for x in samples),'log':log.strip(),
                         'comparison':'No whole-mix reference comparison; reverb/SFX absent, listening unverified'}
    wav=args.output/'opening-dry.wav'
    if not wav.exists():
        with wave.open(str(wav),'wb') as f:f.setnchannels(2);f.setsampwidth(2);f.setframerate(48000);f.writeframes(data)
    write_exact(args.output/'report.json',(json.dumps(report,indent=2)+'\n').encode());print(json.dumps(report,indent=2))

if __name__=='__main__':main()
