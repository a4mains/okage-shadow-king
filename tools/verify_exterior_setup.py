#!/usr/bin/env python3
"""Compare native scene1 setup fields against a completed original outdoor save.

Reference RAM is consumed only by this auditor. Native inputs are the owned
XDB/XMT and native variables from the completed kitchen exit.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
from verify_event import write_exact

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native', type=Path, default=Path('build/native/okage-assets'))
    parser.add_argument('--reference-ram', type=Path,
                        default=Path('local/research/exterior-load180-v1-ee.bin'))
    parser.add_argument('--output', type=Path, default=Path('local/exterior-setup-validation'))
    args = parser.parse_args()
    database = Path('local/xpf-decoded/FLDDATA/FLDSET.XDB')
    script = Path('local/xpf-decoded/FLDDATA/EVENT/EVDATA.XPF/e_field.xmt')
    native_variables = Path('local/native-kitchen-exit-5200-debug-v4.png.variables.bin')
    variables = bytearray(native_variables.read_bytes())
    if len(variables) != 0x1000:
        raise ValueError('Native variable backing extent')
    variables[0x10:0x110] = bytes(256)
    # Scene constructor 0x131d28: scene/entrance, party-enabled input gate.
    for index, value in ((0, 1), (1, 1), (6, 0)):
        struct.pack_into('<i', variables, 0x510 + index*4, value)
    ram = args.reference_ram.read_bytes()
    if len(ram) != 32*1024*1024:
        raise ValueError('Expected a complete 32MiB EE RAM image')
    word = lambda at: struct.unpack_from('<I', ram, at)[0]
    field = word(0x2d9bf4)
    if not 0x100000 <= field <= len(ram)-0x800:
        raise ValueError('Original field pointer bounds')
    game = word(field+0x14)
    if not 0x100000 <= game <= len(ram)-0x60:
        raise ValueError('Original game pointer bounds')
    if (word(game+0x44), word(game+0x4c), word(field+0x410)) != (1, 1, 60):
        raise ValueError('Reference is not scene1 entrance1 startup60')
    args.output.mkdir(parents=True, exist_ok=True)
    values = args.output/'native.variables.bin'
    state = args.output/'native.field.bin'
    write_exact(values, variables)
    subprocess.run([str(args.native), 'field-setup', str(database), '1', str(script),
                    str(values), str(state)], check=True)
    actual = state.read_bytes()
    if len(actual) != 0x800:
        raise ValueError('Native field setup extent')
    compared = []
    for name, offset, size in (('settings', 0x400, 48), ('npc_enable', 0x1a8, 256)):
        if actual[offset:offset+size] != ram[field+offset:field+offset+size]:
            raise ValueError('Original/native mismatch: ' + name)
        compared.append({'name': name, 'offset': offset, 'exact_bytes': size})
    report = {
        'scope': 'Scene1 entry-zero settings and NPC enables; not construction, rendering or playability',
        'scene': 1, 'entrance': 1, 'all_compared_bytes_exact': True,
        'compared': compared, 'settings': list(struct.unpack_from('<12I', actual, 0x400)),
        'inputs': [{'path': str(p), 'sha256': sha(p)} for p in
                   (database, script, native_variables, args.reference_ram, args.native)],
        'native_variables_sha256': sha(values), 'native_field_sha256': sha(state),
        'limits': ['Native game variables still originate in the diagnostic opening',
                   'Reference field contains pointers and later runtime state outside the compared spans',
                   'No captured RAM is passed to the native implementation'],
    }
    write_exact(args.output/'report.json', (json.dumps(report, indent=2)+'\n').encode())
    print(json.dumps(report, indent=2))

if __name__ == '__main__':
    main()
