#!/usr/bin/env python3
"""Import the runtime field database from an owned disc, preserving other copies."""
import argparse
import hashlib
from pathlib import Path
from inspect_disc import Disc

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('disc', type=Path)
    parser.add_argument('--output', type=Path,
                        default=Path('local/xpf-decoded/FLDDATA/FLDSET.XDB'))
    args = parser.parse_args()
    disc = Disc(args.disc)
    try:
        matches = [f for f in disc.inventory() if f['path'] == 'FLDDATA/FLDSET.XDB']
        if len(matches) != 1:
            raise ValueError('Expected one runtime FLDDATA/FLDSET.XDB')
        source = matches[0]
        data = disc.read(source['lba'], source['size'])
    finally:
        disc.close()
    if args.output.is_symlink():
        raise ValueError('Refusing symlink output')
    if args.output.exists():
        if args.output.read_bytes() != data:
            raise ValueError('Refusing to replace a different database')
    else:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open('xb') as stream:
            stream.write(data)
    print(f'{source["path"]}: {len(data)} bytes; SHA256 {hashlib.sha256(data).hexdigest()}')

if __name__ == '__main__':
    main()
