#!/usr/bin/env python3
"""Newgame: does a native Session equal an unchanged-original MINIMAL-HOOK control execution exactly?

usage: newgame_control_exactness.py CONTROL_RUN_DIR NATIVE_REPORT_DIR [--json OUT]

CONTROL_RUN_DIR is an `okage-startup-oracle --newgame-chapter [--name-no-first] --minimal` run (only the title update,
0x124548, 0x11a1c8 and 0x133830 hooked; no rand() breakpoints). At each checkpoint the control took, compares:
  * the Session RNG state (control rand_state_via_ptr) with the native report's last rng.jsonl state,
  * the session state (image counter, command, published name, temporary-bank hash, fade, mode parent),
  * every native KNOWN VRAM nibble with the control's own GS memory dump (fill 0x00),
and the per-call 0x11a1c8 state sequence (phase, length, selection, Yes/No at each return) with the native name-frames rows.
Exit 0 iff everything is equal and complete (a checkpoint missing on either side fails). Research tool.
"""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import newgame_original_extract as ex  # noqa: E402

CHECKPOINTS = ('retire-successor-entry', 'name-constructor-return', 'name-edited', 'name-confirmed',
               'chapter-constructor-return', 'field-constructor-entry')

def run(control, native):
    control, native = Path(control), Path(native)
    meta = json.loads((control / 'run-meta.json').read_text())
    if not meta.get('minimal') or not meta.get('newgame_chapter') or meta.get('missing_checkpoints'):
        raise ValueError('not a complete --newgame-chapter --minimal control run')
    out, ok = {'control': str(control), 'native': str(native), 'checkpoints': {}}, True
    for cp in CHECKPOINTS:
        st = json.loads((control / 'checkpoints' / cp / 'state.jsonl').read_text().splitlines()[0])
        nd = native / cp
        rng = json.loads((nd / 'rng.jsonl').read_text().splitlines()[-1])['state']
        sess = json.loads((nd / 'session.json').read_text())
        dump = (control / 'checkpoints' / cp / 'vram.bin').read_bytes()
        nb, nk = (nd / 'vram.bytes').read_bytes(), (nd / 'vram.known').read_bytes()
        mism = sum(1 for a in range(len(nk)) if nk[a] and ((nb[a] ^ dump[a]) & ((0x0F if nk[a] & 1 else 0) | (0xF0 if nk[a] & 2 else 0))))
        r = {'rng_state_equal': st['rand_state_via_ptr'] == rng, 'session_equal': ex.session_state(st) == sess,
             'known_bytes': sum(1 for k in nk if k), 'known_nibble_mismatches': mism}
        r['equal'] = r['rng_state_equal'] and r['session_equal'] and mism == 0
        ok &= r['equal']
        out['checkpoints'][cp] = r
    ev = [json.loads(l) for l in (control / 'events.jsonl').read_text().splitlines()]
    cseq = [[e['name_entry']['phase'], e['name_entry']['length_a8'], e['name_entry']['selection_cc'], e['name_entry']['yes_no_d8']]
            for e in ev if e['ev'] == 'exit' and e.get('name') == 'name_input']
    nseq = [[r['phase_out'], r['length_out'], r['selection_out'], r['yes_no_out']]
            for r in (json.loads(l) for l in (native / 'name-frames.jsonl').read_text().splitlines()) if r['manager_ran']]
    out['name_call_sequence_equal'] = cseq == nseq
    out['name_calls'] = [len(cseq), len(nseq)]
    out['first_sequence_difference'] = next((i for i, (a, b) in enumerate(zip(cseq, nseq)) if a != b), None)
    ok &= cseq == nseq
    out['native_matches_control_execution'] = ok
    return (0 if ok else 1), out

def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    js = None
    if '--json' in argv:
        i = argv.index('--json'); js = argv[i + 1]; argv = argv[:i] + argv[i + 2:]
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        code, r = run(*argv)
    except (ValueError, OSError, KeyError) as exc:
        print('malformed:', exc, file=sys.stderr)
        return 2
    print(json.dumps(r, indent=1))
    if js:
        Path(js).write_text(json.dumps(r, indent=1) + '\n')
    return code

if __name__ == '__main__':
    sys.exit(main())
