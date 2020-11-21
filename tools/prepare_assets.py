#!/usr/bin/env python3
"""Prepare the observed USA disc revision without modifying the input archive."""
import argparse
import json
import shutil
import tempfile
import zipfile
from pathlib import Path

from inspect_disc import inspect, sha256

ROOT = Path(__file__).resolve().parents[1]
BIN = 'Okage - Shadow King (USA).bin'
CUE = 'Okage - Shadow King (USA).cue'
HASH = '08623a1400e2610aecd0247ae7440c97501495c6f819f675494e44de20b62d2a'
ELF_HASH = '2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('archive', type=Path)
    args = p.parse_args()
    out = ROOT / 'local/disc'
    out.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.archive) as z:
        if sorted(z.namelist()) != sorted([BIN, CUE]):
            raise ValueError('Expected exactly the observed USA BIN/CUE pair')
        if z.getinfo(BIN).file_size != 524481888 or z.getinfo(CUE).file_size > 4096:
            raise ValueError('Unexpected archive member sizes')
        cue = z.read(CUE)
        expected = f'FILE "{BIN}" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n'
        if cue.decode('ascii').replace('\r\n', '\n') != expected:
            raise ValueError('Unexpected track layout')
        dest = out / BIN
        if not dest.exists():
            with tempfile.TemporaryDirectory(dir=out) as tmp:
                candidate = Path(tmp) / BIN
                with z.open(BIN) as src, candidate.open('xb') as dst:
                    shutil.copyfileobj(src, dst)
                if sha256(candidate) != HASH:
                    raise ValueError('Disc hash differs from the observed USA revision')
                candidate.rename(dest)
        elif sha256(dest) != HASH:
            raise ValueError('Existing local disc has a different hash; not overwriting')
        cue_path = out / CUE
        if cue_path.exists():
            if cue_path.read_bytes() != cue:
                raise ValueError('Existing cue differs; not overwriting')
        else:
            with cue_path.open('xb') as f:
                f.write(cue)
    report = inspect(dest, ROOT / 'local/extracted')
    if report['elf']['sha256'] != ELF_HASH:
        raise ValueError('Unexpected boot ELF hash')
    target = ROOT / 'local/disc-report.json'
    target.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Prepared {len(report["files"])} file records and {len(report["extracted"])} research files.')
    print(f'ELF: {report["elf"]["sha256"]}')
    print(f'Report: {target}')

if __name__ == '__main__':
    main()
