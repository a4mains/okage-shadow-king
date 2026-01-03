#!/usr/bin/env python3
"""Import the hash-pinned owned15c3c8 enemy command table, without executable code.

OKBECMD1: little-endian <8s4I32s>, version1/rows84/slots7/cellbytes4/ELF SHA256;
then588 unchanged32-bit cells. Runtime command lookup reads each low signed16.
246d90..2476c0 is bounded by the independently referenced15c1b8 switch table
(15c214). This is an authored region bound, not an original row bounds check.
The manifest binds the source, entire payload and resulting artifact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
from import_battle_data import ELF_SHA256, ElfImage

START, END = 0x246d90, 0x2476c0
ROWS, SLOTS = 84, 7
HEADER = struct.Struct('<8s4I32s')
MAGIC = b'OKBECMD1'
#Independent15c1b8 dispatch targets, not command cells or runtime substitutions.
BOUNDARY = (0x15c228, 0x15c234, 0x15c26c, 0x15c29c, 0x15c25c,
            0x15c2c8, 0x15c2d4, 0x15c2fc, 0x15c328, 0x15c358)

def sha256(data):
    return hashlib.sha256(data).hexdigest()

def extract_payload(image):
    if image.read(END, 40) != struct.pack('<10I', *BOUNDARY):
        raise ValueError('Enemy command region end switch table differs')
    payload = image.read(START, END-START)
    if len(payload) != ROWS*SLOTS*4:
        raise ValueError('Truncated enemy command table')
    return payload

def import_data(elf):
    if sha256(elf) != ELF_SHA256:
        raise ValueError('Owned ELF SHA256 mismatch')
    payload = extract_payload(ElfImage(elf))
    artifact = HEADER.pack(MAGIC, 1, ROWS, SLOTS, 4, bytes.fromhex(ELF_SHA256)) + payload
    return artifact, {'schema':'okage-battle-enemy-commands-v1', 'source_elf_sha256':ELF_SHA256,
                      'source_range':[START,END], 'rows':ROWS, 'slots':SLOTS, 'cell_bytes':4,
                      'payload_sha256':sha256(payload), 'artifact_sha256':sha256(artifact),
                      'artifact_bytes':len(artifact), 'lookup_pc':'0x15c614..0x15c648',
                      'boundary_reference_pc':'0x15c214',
                      'scope':'Static authored data; original helper has no row check; no RNG or chooser execution'}

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--manifest',type=Path)
    args=parser.parse_args(argv)
    manifest=args.manifest or args.output.with_name(args.output.name+'.manifest.json')
    if args.output.absolute()==manifest.absolute():
        raise ValueError('Artifact and manifest paths must differ')
    for path in (args.output,manifest):
        if path.exists() or path.is_symlink():
            raise ValueError(f'Refusing existing or symlink output: {path}')
    artifact,metadata=import_data(args.elf.read_bytes())
    metadata['source_path']=str(args.elf)
    metadata['importer_sha256']=sha256(Path(__file__).read_bytes())
    for path in (args.output,manifest):
        path.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('xb') as stream:
        stream.write(artifact)
    with manifest.open('x') as stream:
        json.dump(metadata,stream,indent=2,sort_keys=True);stream.write('\n')
    return 0

if __name__=='__main__':
    try:
        raise SystemExit(main())
    except (OSError,ValueError,struct.error) as error:
        raise SystemExit(f'import_battle_enemy_commands: {error}')
