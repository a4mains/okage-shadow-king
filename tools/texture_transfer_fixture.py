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

V2_PSMS = (0, 19, 20, 36, 44)

def render_v2(doc):
    """Observer version 2 (Startup): two complementary-fill runs per case, written
    nibble masks.  A case with sub-nibble disagreement (anomalies != 0) is a
    research finding, not a fixture, and is refused."""
    if not isinstance(doc.get("cases"), list) or not 0 < len(doc["cases"]) <= MAX_CASES:
        raise FixtureError(f"'cases' must hold 1..{MAX_CASES} entries")
    if doc.get("fills") != [0, 255]:
        raise FixtureError("v2 observer output must use complementary fills [0, 255]")
    if doc.get("code_tables_preserved") is not True:
        raise FixtureError("observer did not preserve the retail code/tables")
    lines = []
    for idx, c in enumerate(doc["cases"]):
        where = f"case[{idx}]"
        if not isinstance(c, dict):
            raise FixtureError(f"{where}: not an object")
        n = _int(c, "case", 0, 1000000, where)
        kind = {"allocated": 0, "descriptor": 2}.get(c.get("kind"))
        if kind is None:
            raise FixtureError(f"{where}: unknown kind {c.get('kind')!r}")
        psm = _int(c, "psm", 0, 255, where)
        if psm not in V2_PSMS:
            raise FixtureError(f"{where}: unsupported psm {psm}")
        w = _int(c, "w", 1, 512, where)
        h = _int(c, "h", 1, 512, where)
        x = _int(c, "x", 0, 2047, where)
        y = _int(c, "y", 0, 2047, where)
        tbp = _int(c, "tbp", 0, 0x3FFF, where)
        tbw = _int(c, "tbw", 0, 0x3F, where)
        pattern = _int(c, "pattern", 0, 3, where)
        seed = _int(c, "seed", 0, 0xFFFFFFFF, where)
        pre_lo = _int(c, "pre_lo", 0, VRAM_BYTES, where)
        pre_len = _int(c, "pre_len", 0, VRAM_BYTES, where)
        if pre_lo + pre_len > VRAM_BYTES:
            raise FixtureError(f"{where}: preload window outside VRAM")
        if _int(c, "anomalies", 0, VRAM_BYTES, where) != 0:
            raise FixtureError(f"{where}: observer reported sub-nibble write-mask anomalies")
        desc = c.get("descriptor")
        if (not isinstance(desc, list) or len(desc) != 2 or
                any(isinstance(v, bool) or not isinstance(v, int) or not 0 <= v <= 0xFFFFFFFF for v in desc)):
            raise FixtureError(f"{where}: descriptor must be [lo, hi] 32-bit words")
        lo, hi = desc
        if (lo & 0x3FFF) != tbp or ((lo >> 14) & 0x3F) != tbw or (hi & 0x3F) != psm:
            raise FixtureError(f"{where}: tbp/tbw/psm disagree with the descriptor words")
        written = c.get("written")
        if not isinstance(written, list) or len(written) > VRAM_BYTES:
            raise FixtureError(f"{where}: 'written' must be a list")
        if _int(c, "written_count", 0, VRAM_BYTES, where) != len(written):
            raise FixtureError(f"{where}: written_count disagrees with the list")
        seen = set()
        body = []
        for t in written:
            if (not isinstance(t, list) or len(t) != 3 or
                    any(isinstance(v, bool) or not isinstance(v, int) for v in t)):
                raise FixtureError(f"{where}: written entries must be [address, value, mask]")
            a, v, m = t
            if not 0 <= a < VRAM_BYTES or not 0 <= v <= 255 or m not in (1, 2, 3):
                raise FixtureError(f"{where}: written entry {t} out of range")
            if v & ~((0x0F if m & 1 else 0) | (0xF0 if m & 2 else 0)):
                raise FixtureError(f"{where}: written entry {t} has bits in an unwritten nibble")
            if a in seen:
                raise FixtureError(f"{where}: duplicate written address {a}")
            seen.add(a)
            body.append(f"{a} {v} {m}\n")
        lines.append(f"case2 {n} {kind} {psm} {w} {h} {x} {y} {tbp} {tbw} {pattern} {seed} {pre_lo} {pre_len} "
                     f"{lo} {hi} {len(written)}\n")
        lines.extend(body)
    return "".join(lines)

def render(doc):
    if isinstance(doc, dict) and doc.get("schema") == "okage-texture-transfer-v2":
        return render_v2(doc)
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
