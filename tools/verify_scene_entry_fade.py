#!/usr/bin/env python3
"""Compare exact1 scene-entry brightness request and 60 original clock updates."""
import argparse
import json
from pathlib import Path
import subprocess
from prepare_assets import ELF_HASH
from verify_event import digest,write_exact

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();a.output.mkdir(parents=True,exist_ok=False)
    elf=Path('local/extracted/SCUS_971.29');oracle=Path('build/play-research/okage-oracle/okage-pose-oracle')
    if digest(elf)!=ELF_HASH:raise ValueError('Wrong ELF')
    original=a.output/'original.bin';command=[str(oracle),'scene-entry-fade',str(elf),str(original)]
    with (a.output/'original.log').open('w') as log:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True)
    if original.stat().st_size!=3*61*5*4:raise ValueError('Original extent')
    artifacts=[elf,oracle,original];commands=[]
    for build in ['native-release','native','model-sanitize']:
        binary=Path('build')/build/'okage-assets';output=a.output/(build+'.bin')
        cmd=[str(binary),'scene-entry-fade',str(output)];subprocess.run(cmd,check=True);commands.append(cmd)
        if output.read_bytes()!=original.read_bytes():raise ValueError('Scene entry fade differs: '+build)
        artifacts += [binary,output]
    report=dict(scope=__doc__,elf_sha256=ELF_HASH,original_command=command,original_exit_code=0,
                native_commands=commands,initial_brightness=[0,.5,1],samples=183,exact_words=915,
                isolated_boundaries=['0x114ac0 captured-frame transition','0x18fb68 overlay enable',
                                     '0x18fbf8 overlay disable','0x18fc10 RGBA submission'],
                sha256={str(f):digest(f) for f in artifacts},
                limit='Validates request/clock state; entry call order recovered from0x132720. No full GS blending claim.')
    write_exact(a.output/'report.json',(json.dumps(report,indent=2)+'\n').encode())
    print('Exact scene-entry fade: 183 samples / 915 words in three builds')

if __name__=='__main__':main()
