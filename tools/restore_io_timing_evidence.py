#!/usr/bin/env python3
"""Restore: is fixed-traversal RNG parity a property of the original? (current-time analysis of retained runs)

usage: restore_io_timing_evidence.py NATIVE_REPORT_DIR [--json OUT]

Counts Session rand() draws after the Retire stop from the Session generator state (libc LCG
state*0x41c64e6d+0x3039, the same in every run from the identical 9,504-draw prefix) in:
  * the hooked original capture (analysis/manager-frames.jsonl, checkpoint states),
  * the original MINIMAL-HOOK control run of the same unchanged code, disc, card setup and pad stream
    (runs/control-minimal-fill00/checkpoints/*/state.jsonl, rand_state_via_ptr),
  * a native report (rng.jsonl at empty-ready and second-title-return),
and predicts each run's counts from ONE model: the constructor pass (32 draws), then per traversal the pass
for the status 11c2c8 published two traversals earlier (510: 48, 530: 107, 540: 87, after Cancel: 87),
using each run's own observed card readiness (info/ready traversals; the control's from the observer
report, the Restore I/O timing evidence: state 530 at call 2 and 540 at call 5, hooked 3 and 7).
Exit 0 iff every run's counts equal the model's prediction for its readiness.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ORIG = ROOT / "local/research/restore-original-v1"
PASS = {510: 48, 530: 107, 540: 87, -1: 87}
CANCEL_T, DEATH_T, READY_T = 18, 50, 17

def lcg_steps(start, target, limit=200000):
    s, n = start, 0
    while n < limit:
        if s == target:
            return n
        s = (s * 0x41C64E6D + 0x3039) & 0xFFFFFFFF
        n += 1
    raise ValueError("state not reached")

def predicted(info, ready, upto):
    def status(t):
        return -1 if t >= CANCEL_T else 540 if t >= ready else 530 if t >= info else 510
    return 32 + sum(PASS[status(max(t - 2, 0))] for t in range(1, upto + 1))

def readiness(frames):
    info = next(r["t"] for r in frames if r["update_state_out"] == 530)
    ready = next(r["t"] for r in frames if r["update_state_out"] == 540)
    return info, ready

def run(native, out=None):
    analysis = ORIG / "analysis"
    prefix_state = json.loads((analysis / "retire-successor-entry/rng.jsonl").read_text().splitlines()[-1])["state"]
    runs = {}
    o_frames = [json.loads(l) for l in (analysis / "manager-frames.jsonl").read_text().splitlines()]
    o_rng = {cp: json.loads((analysis / cp / "rng.jsonl").read_text().splitlines()[-1])["state"]
             for cp in ("empty-ready", "second-title-return")}
    runs["original-hooked"] = (readiness(o_frames), o_rng)
    ctl = {}
    for cp in ("empty-ready", "second-title-return"):
        rows = [json.loads(l) for l in (ORIG / "runs/control-minimal-fill00/checkpoints" / cp / "state.jsonl").read_text().splitlines()]
        ctl[cp] = next(r["rand_state_via_ptr"] for r in rows if "rand_state_via_ptr" in r)
    hooked_calls, control_calls = (3, 7), (2, 5)
    runs["original-control"] = ((control_calls[0] - 1, control_calls[1] - 1), ctl)
    n_frames = [json.loads(l) for l in (Path(native) / "manager-frames.jsonl").read_text().splitlines()]
    n_rng = {cp: json.loads((Path(native) / cp / "rng.jsonl").read_text().splitlines()[-1])["state"]
             for cp in ("empty-ready", "second-title-return")}
    runs["native"] = (readiness(n_frames), n_rng)
    if readiness(o_frames) != (hooked_calls[0] - 1, hooked_calls[1] - 1):
        raise ValueError("hooked readiness does not match the observer report's call numbering")
    result, ok = {}, True
    for name, ((info, ready), states) in runs.items():
        er = lcg_steps(prefix_state, states["empty-ready"])
        st = lcg_steps(prefix_state, states["second-title-return"])
        pe, ps = predicted(info, ready, READY_T), predicted(info, ready, DEATH_T)
        result[name] = {"card_info_t": info, "card_ready_t": ready, "draws_to_empty_ready": er,
                        "draws_to_second_title": st, "model_empty_ready": pe, "model_second_title": ps,
                        "model_matches": er == pe and st == ps}
        ok &= er == pe and st == ps
    result["original_runs_agree_at_fixed_traversals"] = (
        result["original-hooked"]["draws_to_second_title"] == result["original-control"]["draws_to_second_title"])
    result["kind"] = "current-time-analysis-of-retained-runs"
    print(json.dumps(result, indent=1), file=out or sys.stdout)
    return (0 if ok else 1), result

def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    out = None
    if "--json" in argv:
        i = argv.index("--json"); out = argv[i + 1]; argv = argv[:i] + argv[i + 2:]
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    code, result = run(argv[0])
    if out:
        Path(out).write_text(json.dumps(result, indent=1) + "\n")
    return code

if __name__ == "__main__":
    sys.exit(main())
