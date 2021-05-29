#!/usr/bin/env python3
"""Compare owned village map body construction with a completed original snapshot.

Snapshot data is only an auditor input. Checks the three map bodies' ordering,
geometry, position and body parameters; excludes NPC motion and collision pairs.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zipfile
import zlib
from verify_event import digest, write_exact
from prepare_assets import ELF_HASH

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    elf=Path('local/extracted/SCUS_971.29')
    if digest(elf)!=ELF_HASH:raise ValueError('Wrong ELF')
    prefix=Path('local/reference-village-return-answer-480-v1')
    run=Path(str(prefix)+'.run.json');metadata=json.loads(run.read_text())
    if metadata['exit_code']!=0:raise ValueError('Incomplete original capture')
    snapshot=Path(str(prefix)+'.state.zip')
    with zipfile.ZipFile(snapshot) as z:info=z.getinfo('ram')
    if info.compress_type!=93 or info.file_size!=64*1024*1024:raise ValueError('Snapshot format')
    with snapshot.open('rb') as stream:
        stream.seek(info.header_offset);header=stream.read(30)
        if header[:4]!=b'PK\x03\x04':raise ValueError('ZIP header')
        name,extra=struct.unpack_from('<HH',header,26);stream.seek(name+extra,1);compressed=stream.read(info.compress_size)
    ram=subprocess.run(['zstd','-dc'],input=compressed,stdout=subprocess.PIPE,check=True).stdout
    if len(ram)!=info.file_size or zlib.crc32(ram)!=info.CRC:raise ValueError('Snapshot CRC')
    def words(at,count):
        if not 0<=at<=32*1024*1024-count*4:raise ValueError('Snapshot address')
        return list(struct.unpack_from('<'+'I'*count,ram,at))
    word=lambda at:words(at,1)[0]
    if word(0x337ef0)!=12:raise ValueError('Changed original body count')
    objects,bodies=word(0x21301c),word(0x337ef8)
    # Independently reviewed XMP placement -> original instance allocation.
    ids=[(89,23),(91,25),(92,26)]
    expected=[]
    for index,(placement,instance) in enumerate(ids):
        obj=objects+instance*60
        if struct.unpack_from('<H',ram,obj+50)[0]!=index:raise ValueError('Instance collision association')
        body=bodies+index*176
        if word(body+4)!=2:raise ValueError('Original shape count')
        row=[placement,instance,index,word(body+12),word(body+20),word(body+24),word(body)]+words(word(body+32),4)
        shapes=word(body+8)
        for j in range(2):
            flags,geometry=words(shapes+j*8,2);kind=flags&7
            first=words(geometry,4);second=words(geometry+16,4) if kind==2 else [0]*4
            radius=word(geometry+(32 if kind==2 else 16)) if kind else 0
            if kind not in [0,1,2]:raise ValueError('Shape type')
            row += [flags]+first+second+[radius]
        expected+=row
    original=a.output/'original.bin';write_exact(original,struct.pack('<'+'I'*len(expected),*expected))
    game_map=Path('local/xpf-decoded/FLDDATA/MAP/VIL/VSTART_T.XPF/a.xmp')
    artifacts=[elf,run,snapshot,game_map,original];commands=[]
    for build in ['native-release','native','model-sanitize']:
        binary=Path('build')/build/'okage-assets';output=a.output/(build+'.bin')
        command=[str(binary),'map-collision',str(game_map),str(output)]
        subprocess.run(command,check=True);commands.append(command)
        if output.read_bytes()!=original.read_bytes():
            got=list(struct.unpack('<'+'I'*(output.stat().st_size//4),output.read_bytes()))
            raise ValueError('Map collision differences: '+str([(i,hex(x),hex(y)) for i,(x,y) in enumerate(zip(expected,got)) if x!=y][:20]))
        artifacts += [binary,output]
    report=dict(scope=__doc__,elf_sha256=ELF_HASH,source_run=metadata,
                original_collision_count=12,map_bodies=3,exact_words=len(expected),commands=commands,
                ram_sha256=hashlib.sha256(ram).hexdigest(),sha256={str(f):digest(f) for f in artifacts},
                static_recovery=['0x1bba88: definition offset0x80 selects collision type; definition-indexed scale source',
                                 '0x1b86e8: floor point plus sphere/capsule, radius and priority',
                                 '0x1b8a80: placement-order body allocation and visibility index-zero exemption'],
                limits=['Original body initialization is observed after gameplay, not an isolated constructor call.',
                        'Three unchanged map positions match owned placements exactly; actor state is outside this comparison.',
                        'Sphere/capsule dynamic pair dispatch remains guarded until separately recovered.'])
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print('Three map bodies match original snapshot exactly in three builds:',len(expected),'words')

if __name__=='__main__':main()
