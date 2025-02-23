#!/usr/bin/env python3
"""Convert texture-transfer observer JSON (research evidence) into the text
fixture read by okage-texture-transfer-compare.

Usage: texture_transfer_fixture.py in.json out.txt

The descriptor is two 32-bit words [lo, hi] as emitted by the observer. The
generator validates every field, refuses to overwrite an existing output and
writes atomically (temporary file in the output directory, then link, so a
failure never leaves a partial or replaced output)."""
import json
import os
import sys
import tempfile

KINDS = ("image", "clut")
MAX_CASES = 4096
VRAM_BYTES = 4 * 1024 * 1024

class FixtureError(Exception):
    pass

def _int(c, key, lo, hi, where):
    if key not in c:
        raise FixtureError(f"{where}: missing key '{key}'")
    v = c[key]
    if isinstance(v, bool) or not isinstance(v, int):
        raise FixtureError(f"{where}: '{key}' must be an integer")
    if not lo <= v <= hi:
        raise FixtureError(f"{where}: '{key}'={v} outside [{lo}, {hi}]")
    return v

def render(doc):
    if not isinstance(doc, dict) or not isinstance(doc.get("cases"), list):
        raise FixtureError("top level must be an object with a 'cases' list")
    if not 0 < len(doc["cases"]) <= MAX_CASES:
        raise FixtureError(f"'cases' must hold 1..{MAX_CASES} entries")
    lines = []
    for idx, c in enumerate(doc["cases"]):
        where = f"case[{idx}]"
        if not isinstance(c, dict):
            raise FixtureError(f"{where}: not an object")
        n = _int(c, "case", 0, 1000000, where)
        kind = c.get("kind")
        if kind not in KINDS:
            raise FixtureError(f"{where}: unknown kind {kind!r}")
        psm = _int(c, "psm", 0, 255, where)
        w = _int(c, "w_or_n", 1, 256, where)
        h = _int(c, "h", 0, 256, where)
        rows = _int(c, "clut_alloc_rows", 0, 1024, where)
        if kind == "image":
            if psm not in (0, 19) or h < 1 or rows != 0:
                raise FixtureError(f"{where}: invalid image psm/h/rows")
        elif psm != 0 or h != 0 or rows != ((w + 31) >> 5) << 1:
            raise FixtureError(f"{where}: invalid clut psm/h/rows")
        desc = c.get("descriptor")
        if (not isinstance(desc, list) or len(desc) != 2 or
                any(isinstance(x, bool) or not isinstance(x, int) or not 0 <= x <= 0xFFFFFFFF for x in desc)):
            raise FixtureError(f"{where}: descriptor must be [lo, hi] 32-bit words")
        lo, hi = desc
        tbp = _int(c, "tbp", 0, 0x3FFF, where)
        tbw = _int(c, "tbw", 0, 0x3F, where)
        if (lo & 0x3FFF) != tbp or ((lo >> 14) & 0x3F) != tbw:
            raise FixtureError(f"{where}: tbp/tbw disagree with descriptor lo word")
        touched = c.get("touched")
        if not isinstance(touched, list) or len(touched) > VRAM_BYTES:
            raise FixtureError(f"{where}: 'touched' must be a list")
        seen = set()
        body = []
        for t in touched:
            if (not isinstance(t, list) or len(t) != 2 or
                    any(isinstance(x, bool) or not isinstance(x, int) for x in t)):
                raise FixtureError(f"{where}: touched entries must be [address, value]")
            a, v = t
            if not 0 <= a < VRAM_BYTES or not 0 <= v <= 255:
                raise FixtureError(f"{where}: touched entry {t} out of range")
            if a in seen:
                raise FixtureError(f"{where}: duplicate touched address {a}")
            seen.add(a)
            body.append(f"{a} {v}\n")
        lines.append(f"case {n} {kind} {psm} {w} {h} {rows} {lo} {hi} {tbp} {tbw} {len(touched)}\n")
        lines.extend(body)
    return "".join(lines)

def write_new(path, text):
    """Atomically create `path`; never replace an existing file."""
    if os.path.lexists(path):
        raise FixtureError(f"refusing to overwrite existing output: {path}")
    directory = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(prefix=".fixture-", dir=directory)
    try:
        with os.fdopen(fd, "w") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        os.link(tmp, path)
    except FileExistsError:
        raise FixtureError(f"refusing to overwrite existing output: {path}")
    finally:
        os.unlink(tmp)

def main(argv):
    if len(argv) != 3:
        print(__doc__.splitlines()[2], file=sys.stderr)
        return 2
    try:
        with open(argv[1], "rb") as f:
            raw = f.read(64 * 1024 * 1024 + 1)
        if len(raw) > 64 * 1024 * 1024:
            raise FixtureError("input exceeds size cap")
        try:
            doc = json.loads(raw)
        except (ValueError, RecursionError) as e:
            raise FixtureError(f"malformed JSON: {e}")
        write_new(argv[2], render(doc))
    except (FixtureError, OSError) as e:
        print(f"fixture error: {e}", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv))
