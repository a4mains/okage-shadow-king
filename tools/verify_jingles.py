#!/usr/bin/env python3
"""Import pinned owned jingle resources and validate bounded native audio behavior."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from inspect_disc import Disc
from prepare_assets import ELF_HASH
from verify_event import write_exact
from verify_audio import opening_samples
from build_voice_reference import build, HASHES
from build_spu_reference import REVISION

ASSETS = {'JGL_FLG.XSQ': '5cbfdeda457005fc4dea89774c5477c4bd25af0306cde566c50ef51afe3368a8', 'JGL_STN.XSQ': 'b9c66914a98995a4b428298f3f99b8c566257d02210957e02d3360a97a00f2e3', 'JGL_GET.XSQ': 'b15448498b8a4738cb98d72fda4a595daf6fdc8bee644d732633f631f9e2f407', 'JGL_SPL.XSQ': '80bf7d5c0fdbd10d968a1c2f68fdd4728c4db042dffe18937347cf7c77834bc4', 'JGL_FVC.XSQ': '4fc3e1cab7b36a11bd69c7ee7da64b837259fa1e4179c7644fa0316cc16ebaaf', 'JGL_LUP.XSQ': '4845f7a0c48040a1bdb3058055b32e284fffca5fc91b10aeff7480192a5fb5be', 'JGL_BSC0.XV': 'a2c11debcde59c737fe9217821d4cbaf27f42a83eb4c202c2ea7a97ab654df85'}

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--prepare',action='store_true')
    p.add_argument('--native',type=Path,default=Path('build/native-release/okage-assets'))
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    if digest(Path('local/extracted/SCUS_971.29'))!=ELF_HASH:
        raise ValueError('Different retail executable')
    root=Path('local/audio-input/SNDDATA/BGM')
    if a.prepare:
        entries={e['path']:e for e in json.loads(Path('docs/disc-report.json').read_text())['files']}
        disc=Disc('local/disc/Okage - Shadow King (USA).bin');imported=[]
        try:
            for name,sha in ASSETS.items():
                e=entries['SNDDATA/BGM/'+name];data=disc.read(e['lba'],e['size'])
                if hashlib.sha256(data).hexdigest()!=sha:raise ValueError('Different owned jingle resource')
                write_exact(root/name,data);imported.append(dict(name=name,sha256=sha,lba=e['lba'],size=len(data)))
        finally:disc.close()
        write_exact(a.output/'import.json',(json.dumps(imported,indent=2)+'\n').encode());return
    for name,sha in ASSETS.items():
        if digest(root/name)!=sha:raise ValueError('Different imported jingle resource')
    commands=[['python3','tools/verify_sequence.py','--reference','local/reference-bakery-jingle-300-v1.bin',
               '--sequence',str(root/'JGL_GET.XSQ'),'--native',str(a.native),'--output',str(a.output/'sequence')],
              ['python3','tools/verify_music_bank.py','--reference','local/reference-bakery-jingle-160-v1',
               '--bank',str(root/'JGL_BSC0.XV'),'--native',str(a.native),'--output',str(a.output/'parameters')]]
    for command in commands:subprocess.run(command,check=True,stdout=subprocess.DEVNULL)
    reference=build();_,body,tones=opening_samples((root/'JGL_BSC0.XV').read_bytes())
    metadata=subprocess.check_output([str(a.native),'music-bank',str(root/'JGL_BSC0.XV')],text=True)
    records=[list(map(int,line.split())) for line in metadata.splitlines()]
    if len(tones)!=5 or len(records)!=5:raise ValueError('Shared jingle bank tone extent changed')
    cases=[]
    for i,(tone,row) in enumerate(zip(tones,records)):
        if (tone['offset'],tone['program'])!=(row[3],row[0]):raise ValueError('Tone mapping differs')
        source=a.output/f'tone-{i}.adpcm';write_exact(source,body[tone['offset']:tone['offset']+tone['bytes']])
        args=[str(source),str(row[4]),str(row[5]),'4096','30','8000','16000']
        original=a.output/f'tone-{i}.original.bin';native=a.output/f'tone-{i}.native.bin'
        subprocess.run([str(reference),'voice',*args,str(original)],check=True)
        subprocess.run([str(a.native),'voice-sample',*args,str(native)],check=True)
        if original.read_bytes()!=native.read_bytes():raise ValueError('Jingle tone DSP differs')
        cases.append(dict(tone=i,program=row[0],bytes=native.stat().st_size,sha256=digest(native)))
    routing=Path('local/research/jingle-routing-original-v1.bin');r=tuple(struct.iter_unpack('<27I',routing.read_bytes()))
    if [(row[0],row[1]) for row in r]!=[(i,c) for c in [0xffffffff,10] for i in [20,21,22,23,33,34]]:
        raise ValueError('Routing probe cases/extent')
    if digest(routing)!='f4e1b04a260ae00ad4da049f99f00f9ddcd59f9828c7e988302939985d700355':
        raise ValueError('Changed original routing corpus')
    for row in r:
        index=row[0]-20 if row[0]<24 else row[0]-29
        expected=(0x100+index,128,128,0,128,128,row[1],1,0x8765,0x800000,0x800000,0x2000,1)
        if row[0] not in [20,21,22,23,33,34] or row[1] not in [0xffffffff,10] or row[2:15]!=expected:
            raise ValueError('Original jingle routing differs')
        if row[15:21]!=tuple(0x1234 if i==index else 0x200+i for i in range(6)) or row[21:]!=tuple(0x100+i for i in range(6)):
            raise ValueError('Original registration/playback handles differ')
    report=dict(scope='JGL_GET sequencing and observed voice parameters; five fixed-condition DSP tones; original EE routing with recorded driver boundaries. No whole-scene audio, RPC timing or shared-voice scheduling equivalence claim.',
        assets=ASSETS,native_tool_sha256=digest(a.native),commands=commands,
        sequence=json.loads((a.output/'sequence/report.json').read_text()),
        parameters=json.loads((a.output/'parameters/report.json').read_text()),
        dsp=dict(revision=REVISION,source_hashes=HASHES,cases=cases),
        original_routing=dict(cases=12,words=324,sha256=digest(routing),
            command=['build/play-research/okage-oracle/okage-pose-oracle','jingle-routing','local/extracted/SCUS_971.29',str(routing)],
            boundaries=['0x1708f8 records arguments and returns synthetic handle0x1234','0x170878 records global gains']))
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print('Jingle sequence, live note parameters, five DSP tones and original routing checks passed')

if __name__=='__main__':main()
