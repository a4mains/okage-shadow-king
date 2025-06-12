#!/usr/bin/env python3
"""RETIRE input provenance: audit a staged game-data tree and check runtime read logs.

Subcommands:
  audit STAGE_DIR --manifest-sha256 HEX [--out JSON]
  check-reads STAGE_DIR READ_LOG --manifest-sha256 HEX [--out JSON] [--strict-stage]

Hashes are of the bytes read through any symlink.  Stdlib only.
"""
import argparse
import datetime
import hashlib
import json
import os
import sys
from concurrent.futures import ThreadPoolExecutor

MANIFEST_NAME = ".owned-game-data-manifest.json"
CHUNK = 1 << 20

def hash_file(path):
    """Return (sha256_hex, size) of the bytes read through `path`; raises OSError."""
    h = hashlib.sha256()
    buf = bytearray(CHUNK)
    view = memoryview(buf)
    total = 0
    with open(path, "rb", buffering=0) as f:
        while True:
            n = f.readinto(buf)
            if not n:
                break
            h.update(view[:n])
            total += n
    return h.hexdigest(), total

def hash_many(paths, workers=None):
    """Map path -> (sha, size) or Exception, hashed in parallel."""
    workers = workers or min(8, (os.cpu_count() or 2))

    def one(p):
        try:
            return hash_file(p)
        except OSError as e:
            return e

    with ThreadPoolExecutor(max_workers=workers) as ex:
        return dict(zip(paths, ex.map(one, paths)))

def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()

def load_manifest(stage, pinned):
    """Verify manifest hash; return (manifest_sha, {relpath: entry}, error_or_None)."""
    mpath = os.path.join(stage, MANIFEST_NAME)
    try:
        sha, _ = hash_file(mpath)
    except OSError as e:
        return None, {}, "cannot read manifest: %s" % e
    if sha != pinned.lower():
        return sha, {}, "manifest sha256 %s != pinned %s" % (sha, pinned.lower())
    try:
        with open(mpath, "rb") as f:
            doc = json.load(f)
        entries = {}
        dups = []
        for e in doc["files"]:
            p = e["path"]
            if p in entries:
                dups.append(p)
            entries[p] = {"sha256": e["sha256"].lower()}
    except (ValueError, KeyError, TypeError, AttributeError) as e:
        return sha, {}, "malformed manifest: %r" % (e,)
    if dups:
        return sha, entries, "duplicate manifest paths: %s" % dups[:5]
    return sha, entries, None

def write_out(summary, out):
    text = json.dumps(summary, indent=2, sort_keys=True)
    if out:
        with open(out, "w") as f:
            f.write(text + "\n")
    else:
        print(text)

def list_stage_files(stage):
    found = []
    for root, dirs, files in os.walk(stage):
        for d in list(dirs):
            full = os.path.join(root, d)
            if os.path.islink(full):
                dirs.remove(d)
                files.append(d)
        for fn in files:
            rel = os.path.relpath(os.path.join(root, fn), stage).replace(os.sep, "/")
            if rel != MANIFEST_NAME:
                found.append(rel)
    return found

def cmd_audit(args):
    stage = os.path.abspath(args.stage)
    summary = {"kind": "current-time-audit", "audited_at": utc_now(), "stage": stage,
               "manifest_sha256_pinned": args.manifest_sha256.lower()}
    sha, entries, err = load_manifest(stage, args.manifest_sha256)
    summary["manifest_sha256"] = sha
    if err:
        summary.update(failures=[{"type": "manifest", "detail": err}], ok=False)
        write_out(summary, args.out)
        return 1
    rels = sorted(entries)
    res = hash_many([os.path.join(stage, r) for r in rels])
    failures, symlinks, total, ok_n = [], {}, 0, 0
    for r in rels:
        full = os.path.join(stage, r)
        if os.path.islink(full):
            symlinks[r] = os.path.realpath(full)
        v = res[full]
        if isinstance(v, Exception):
            failures.append({"type": "missing", "path": r, "detail": str(v)})
            continue
        total += v[1]
        if v[0] != entries[r]["sha256"]:
            failures.append({"type": "mismatch", "path": r, "expected": entries[r]["sha256"], "actual": v[0]})
        else:
            ok_n += 1
    unlisted = sorted(set(list_stage_files(stage)) - set(rels))
    for u in unlisted:
        failures.append({"type": "unlisted", "path": u})
    summary.update(
        listed_files=len(rels), verified_ok=ok_n,
        mismatched=sum(f["type"] == "mismatch" for f in failures),
        missing=sum(f["type"] == "missing" for f in failures),
        unlisted_files=unlisted, total_bytes=total,
        symlink_count=len(symlinks), symlink_targets=symlinks,
        failures=failures, ok=not failures)
    write_out(summary, args.out)
    return 0 if not failures else 1

def under(path, base):
    return path == base or path.startswith(base.rstrip(os.sep) + os.sep)

def stage_rel(path, stage_abs, stage_real):
    """Stage-relative path for an opened path, or None if outside the stage."""
    p = os.path.normpath(path if os.path.isabs(path) else os.path.join(os.getcwd(), path))
    for base in (stage_abs, stage_real):
        if under(p, base):
            return os.path.relpath(p, base).replace(os.sep, "/")
    return None

def cmd_check_reads(args):
    stage_abs = os.path.abspath(args.stage)
    stage_real = os.path.realpath(args.stage)
    summary = {"kind": "run-time-read-provenance", "stage": stage_real,
               "manifest_sha256_pinned": args.manifest_sha256.lower()}
    sha, entries, err = load_manifest(stage_real, args.manifest_sha256)
    summary["manifest_sha256"] = sha
    failures, non_stage = [], []
    if err:
        failures.append({"type": "manifest", "detail": err})
        summary.update(reads=0, distinct_files=0, failures=failures, non_stage=[],
                       consumed_assets=[], current_rehash={}, ok=False)
        write_out(summary, args.out)
        return 1
    reads, consumed = 0, {}
    try:
        with open(args.read_log, "r", encoding="utf-8") as f:
            lines = f.read().splitlines()
    except (OSError, UnicodeDecodeError) as e:
        lines = []
        failures.append({"type": "log-unreadable", "detail": str(e)})
    for no, line in enumerate(lines, 1):
        if not line.strip():
            continue
        try:
            rec = json.loads(line)
            path, nbytes, rsha = rec["path"], rec["bytes"], rec["sha256"].lower()
            if not isinstance(path, str) or isinstance(nbytes, bool) or not isinstance(nbytes, int):
                raise TypeError("bad field types")
        except (ValueError, KeyError, TypeError, AttributeError) as e:
            failures.append({"type": "malformed-line", "line": no, "detail": repr(e)})
            continue
        reads += 1
        rel = stage_rel(path, stage_abs, stage_real)
        if rel is None:
            non_stage.append({"line": no, "path": path, "resolved": rec.get("resolved"),
                              "bytes": nbytes, "sha256": rsha})
            if args.strict_stage:
                failures.append({"type": "outside-stage", "line": no, "path": path})
            continue
        if rel == MANIFEST_NAME:
            continue
        ent = entries.get(rel)
        if ent is None:
            failures.append({"type": "not-in-manifest", "line": no, "path": rel})
            continue
        if rsha != ent["sha256"]:
            failures.append({"type": "sha-mismatch", "line": no, "path": rel,
                             "expected": ent["sha256"], "logged": rsha})
            continue
        prev = consumed.get(rel)
        if prev is not None and prev["bytes"] != nbytes:
            failures.append({"type": "size-inconsistent", "line": no, "path": rel})
            continue
        consumed[rel] = {"bytes": nbytes, "sha256": rsha}
    if reads == 0 and not failures:
        failures.append({"type": "empty-log", "detail": "read log contains no reads"})
    # current-time rehash of every consumed file; also validates logged byte counts
    rehash = {"label": "current-time", "checked": len(consumed), "still_match": 0, "differ": []}
    res = hash_many([os.path.join(stage_real, r) for r in sorted(consumed)])
    for r in sorted(consumed):
        v = res[os.path.join(stage_real, r)]
        if isinstance(v, Exception):
            rehash["differ"].append({"path": r, "detail": str(v)})
            continue
        if v[1] != consumed[r]["bytes"]:
            failures.append({"type": "size-mismatch", "path": r,
                             "logged": consumed[r]["bytes"], "actual_size": v[1]})
        if v[0] == consumed[r]["sha256"]:
            rehash["still_match"] += 1
        else:
            rehash["differ"].append({"path": r, "actual": v[0]})
    summary.update(
        reads=reads, distinct_files=len(consumed), failures=failures,
        non_stage=non_stage, current_rehash=rehash,
        consumed_assets=[{"path": r, "sha256": consumed[r]["sha256"]} for r in sorted(consumed)],
        ok=not failures)
    write_out(summary, args.out)
    return 0 if not failures else 1

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("audit")
    a.add_argument("stage")
    a.add_argument("--manifest-sha256", required=True)
    a.add_argument("--out")
    a.set_defaults(fn=cmd_audit)
    c = sub.add_parser("check-reads")
    c.add_argument("stage")
    c.add_argument("read_log")
    c.add_argument("--manifest-sha256", required=True)
    c.add_argument("--out")
    c.add_argument("--strict-stage", action="store_true")
    c.set_defaults(fn=cmd_check_reads)
    args = ap.parse_args(argv)
    return args.fn(args)

if __name__ == "__main__":
    sys.exit(main())
