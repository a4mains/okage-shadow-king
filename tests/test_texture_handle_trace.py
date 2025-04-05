#!/usr/bin/env python3
"""Controls for tools/texture_handle_trace_diff.py and okage-texture-handle-trace.

Native tool: $OKAGE_HANDLE_TRACE, else build/handle-trace-release/okage-texture-handle-trace;
the native tests are skipped when it is missing. See the native handle lifecycle trace schema.
"""
import copy
import hashlib
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import texture_handle_trace_diff as differ  # noqa: E402

def base_line(op, text="x"):
    return {
        "op": op, "text": text, "result": 0,
        "handles": [[0, 0]],
        "records": [{"r": 0, "image": 0, "pal0": -1, "name": "", "hash_slot": -1, "handles": 1, "extra": []}],
        "hash": [], "live_handles": 1, "reserved": [], "free_records": 511, "free_extras": 128,
        "descriptors": [[0, 1]],
        "vram": {"available": 4000, "cursors": [0] * 64, "occupancy_sha256": "aa", "touched": 4, "touched_sha256": "bb"},
        "file_requests": [],
    }

def write_trace(path, lines):
    Path(path).write_text("".join(json.dumps(line) + "\n" for line in lines))

class DifferTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.lines = [base_line(-1, "init"), base_line(0), base_line(1)]

    def run_diff(self, a_lines, b_lines, *extra, a_raw=None, b_raw=None):
        a, b = self.dir / "a.jsonl", self.dir / "b.jsonl"
        if a_raw is None:
            write_trace(a, a_lines)
        else:
            a.write_text(a_raw)
        if b_raw is None:
            write_trace(b, b_lines)
        else:
            b.write_text(b_raw)
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = differ.main([str(a), str(b), *extra])
        return code, out.getvalue(), err.getvalue()

    def test_identical(self):
        code, out, _ = self.run_diff(self.lines, copy.deepcopy(self.lines))
        self.assertEqual(code, 0)
        self.assertIn("IDENTICAL", out)

    def test_each_field_differs(self):
        mutations = {
            "op": 7, "text": "other", "result": 3, "handles": [[1, 0]],
            "records": [], "hash": [[1, 2]], "live_handles": 2, "reserved": [5],
            "free_records": 3, "free_extras": 4, "descriptors": [[0, 2]],
            "file_requests": ["a.xim"],
        }
        for field, value in mutations.items():
            with self.subTest(field=field):
                other = copy.deepcopy(self.lines)
                other[2][field] = value
                code, out, _ = self.run_diff(self.lines, other)
                self.assertEqual(code, 1)
                self.assertIn(f"  {field}:", out)
                self.assertIn("first difference: line 3", out)
        for sub, value in {"available": 1, "cursors": [1] + [0] * 63, "occupancy_sha256": "zz",
                           "touched": 5, "touched_sha256": "cc"}.items():
            with self.subTest(field="vram." + sub):
                other = copy.deepcopy(self.lines)
                other[1]["vram"][sub] = value
                code, out, _ = self.run_diff(self.lines, other)
                self.assertEqual(code, 1)
                self.assertIn(f"  vram.{sub}:", out)
                self.assertIn("first difference: line 2", out)

    def test_first_difference_is_per_field_and_lists_detail(self):
        other = copy.deepcopy(self.lines)
        other[1]["descriptors"] = [[0, 1], [4, 1]]
        other[2]["descriptors"] = [[0, 1], [4, 1]]
        other[2]["result"] = 9
        code, out, _ = self.run_diff(self.lines, other)
        self.assertEqual(code, 1)
        self.assertIn("field descriptors: first at line 2", out)
        self.assertIn("field result: first at line 3", out)
        self.assertIn("extra element [1] only in native", out)

    def test_ignore_is_opt_in(self):
        other = copy.deepcopy(self.lines)
        other[1]["vram"]["touched"] = 99
        other[1]["vram"]["touched_sha256"] = "zz"
        self.assertEqual(self.run_diff(self.lines, other)[0], 1)
        self.assertEqual(self.run_diff(self.lines, other, "--ignore", "vram.touched")[0], 1)
        self.assertEqual(self.run_diff(self.lines, other, "--ignore", "vram.touched", "--ignore", "vram.touched_sha256")[0], 0)
        self.assertEqual(self.run_diff(self.lines, other, "--ignore", "vram")[0], 0)
        code, _, err = self.run_diff(self.lines, other, "--ignore", "nosuchfield")
        self.assertEqual(code, 1)
        self.assertIn("matches no field", err)

    def test_truncation_and_extra_lines(self):
        code, out, _ = self.run_diff(self.lines, self.lines[:2])
        self.assertEqual(code, 1)
        self.assertIn("line count differs: original 3, native 2", out)
        code, out, _ = self.run_diff(self.lines[:1], self.lines)
        self.assertEqual(code, 1)
        self.assertIn("line count differs: original 1, native 3", out)
        code, _, _ = self.run_diff([], [])
        self.assertEqual(code, 0)

    def test_missing_field_is_a_difference(self):
        other = copy.deepcopy(self.lines)
        del other[1]["hash"]
        code, out, _ = self.run_diff(self.lines, other)
        self.assertEqual(code, 1)
        self.assertIn("field present in only one trace", out)

    def test_malformed(self):
        good = "".join(json.dumps(line) + "\n" for line in self.lines)
        for name, raw in {"bad json": good + "{not json\n", "not object": good + "[1,2]\n",
                          "blank line": good + "\n\n", "truncated line": good + '{"op":3,"te'}.items():
            with self.subTest(name):
                self.assertEqual(self.run_diff(None, None, a_raw=raw, b_raw=good)[0], 2)
                self.assertEqual(self.run_diff(None, None, a_raw=good, b_raw=raw)[0], 2)
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = differ.main([str(self.dir / "missing-a"), str(self.dir / "missing-b")])
        self.assertEqual(code, 2)

    def test_usage_error_exit_2(self):
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            self.assertEqual(differ.main(["only-one"]), 2)

HEX_A, HEX_B = "a" * 64, "b" * 64

def accept_line(op, text):
    line = base_line(op, text)
    line["vram"] = {"available": 4000, "cursors": [0] * 64, "occupancy_sha256": HEX_A,
                    "touched": 4, "touched_sha256": HEX_B}
    return line

class AcceptModeTests(unittest.TestCase):
    """--accept SCRIPT: schema, sequence and length gate (negative controls)."""

    SCRIPT = "# comment\ncache 0\n\n  lower 1  \n"

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        (self.dir / "s.txt").write_text(self.SCRIPT)
        self.good = [accept_line(-1, "init"), accept_line(0, "cache 0"), accept_line(1, "lower 1")]

    def run_accept(self, a, b, script=None, raw_a=None, raw_b=None, extra=()):
        pa, pb = self.dir / "a.jsonl", self.dir / "b.jsonl"
        pa.write_text(raw_a if raw_a is not None else "".join(json.dumps(x) + "\n" for x in a))
        pb.write_text(raw_b if raw_b is not None else "".join(json.dumps(x) + "\n" for x in b))
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            code = differ.main([str(pa), str(pb), "--accept", str(script or self.dir / "s.txt"), *extra])
        return code, out.getvalue(), err.getvalue()

    def mutated(self, fn):
        lines = copy.deepcopy(self.good)
        fn(lines)
        return lines

    def test_identical_schema_valid_traces_pass(self):
        code, out, _ = self.run_accept(self.good, copy.deepcopy(self.good))
        self.assertEqual(code, 0)
        self.assertIn("script operations: 2", out)

    def test_real_value_mutation_is_different_not_malformed(self):
        for name, fn in {
            "result": lambda L: L[2].update(result=5),
            "vram.touched": lambda L: L[2]["vram"].update(touched=5),
            "vram.hash": lambda L: L[1]["vram"].update(touched_sha256=HEX_A),
            "cursor": lambda L: L[2]["vram"]["cursors"].__setitem__(3, 1),
            "descriptor": lambda L: L[2].update(descriptors=[[0, 2]]),
        }.items():
            with self.subTest(name):
                code, out, _ = self.run_accept(self.good, self.mutated(fn))
                self.assertEqual(code, 1, out)
                self.assertIn("DIFFERENT", out)

    def test_empty_and_truncated_captures_are_malformed(self):
        for name, lines in {"empty": [], "init-only": self.good[:1], "prefix": self.good[:2]}.items():
            with self.subTest(name):
                # Two equally truncated captures must not pass.
                self.assertEqual(self.run_accept(lines, copy.deepcopy(lines))[0], 2)
                self.assertEqual(self.run_accept(self.good, lines)[0], 2)
                self.assertEqual(self.run_accept(lines, self.good)[0], 2)

    def test_extra_lines_are_malformed(self):
        longer = self.good + [accept_line(2, "cache 0")]
        self.assertEqual(self.run_accept(longer, copy.deepcopy(longer))[0], 2)

    def test_empty_objects_are_malformed(self):
        self.assertEqual(self.run_accept([{}, {}, {}], [{}, {}, {}])[0], 2)

    def test_missing_field_on_both_sides_is_malformed(self):
        for key in ("handles", "vram", "file_requests", "result"):
            with self.subTest(key):
                broken = self.mutated(lambda L, k=key: L[2].pop(k))
                self.assertEqual(self.run_accept(broken, copy.deepcopy(broken))[0], 2)
                self.assertEqual(self.run_accept(self.good, broken)[0], 2)

    def test_unknown_and_dotted_keys_are_malformed(self):
        for key in ("extra", "vram.available"):
            with self.subTest(key):
                broken = self.mutated(lambda L, k=key: L[2].update({k: 1}))
                self.assertEqual(self.run_accept(broken, copy.deepcopy(broken))[0], 2)
        nested = self.mutated(lambda L: L[2]["vram"].update(extra=1))
        self.assertEqual(self.run_accept(nested, copy.deepcopy(nested))[0], 2)

    def test_wrong_types_are_malformed(self):
        cases = {
            "bool op": lambda L: L[1].update(op=False),
            "float result": lambda L: L[2].update(result=1.5),
            "bool result": lambda L: L[2].update(result=True),
            "string live_handles": lambda L: L[2].update(live_handles="1"),
            "bool in cursors": lambda L: L[2]["vram"]["cursors"].__setitem__(0, False),
            "short cursors": lambda L: L[2]["vram"].update(cursors=[0] * 63),
            "bad hash": lambda L: L[2]["vram"].update(occupancy_sha256="AA" * 32),
            "pair arity": lambda L: L[2].update(handles=[[1, 2, 3]]),
            "unsorted handles": lambda L: L[2].update(handles=[[2, 0], [1, 1]]),
            "file_requests ints": lambda L: L[2].update(file_requests=[1]),
        }
        for name, fn in cases.items():
            with self.subTest(name):
                broken = self.mutated(fn)
                self.assertEqual(self.run_accept(broken, copy.deepcopy(broken))[0], 2)

    def test_sequence_and_text_must_follow_script(self):
        for name, fn in {
            "reordered": lambda L: L.__setitem__(slice(1, 3), [L[2], L[1]]),
            "duplicate op": lambda L: L[2].update(op=0),
            "skipped op": lambda L: L[2].update(op=2),
            "wrong text": lambda L: L[2].update(text="lower 0"),
            "init not first": lambda L: L[0].update(op=0),
        }.items():
            with self.subTest(name):
                broken = self.mutated(fn)
                self.assertEqual(self.run_accept(broken, copy.deepcopy(broken))[0], 2)

    def test_duplicate_keys_and_nonfinite_numbers_are_malformed(self):
        good_text = "".join(json.dumps(x) + "\n" for x in self.good)
        dup = good_text.replace('"op": 1,', '"op": 1, "op": 1,')
        self.assertNotEqual(dup, good_text)
        self.assertEqual(self.run_accept(None, None, raw_a=dup, raw_b=good_text)[0], 2)
        self.assertEqual(self.run_accept(None, None, raw_a=good_text, raw_b=dup)[0], 2)
        for token in ("NaN", "Infinity", "-Infinity"):
            with self.subTest(token):
                bad = good_text.replace('"result": 0', f'"result": {token}', 1)
                self.assertNotEqual(bad, good_text)
                self.assertEqual(self.run_accept(None, None, raw_a=bad, raw_b=bad)[0], 2)

    def test_same_file_and_inconsistent_cross_fields_are_malformed(self):
        p = self.dir / "same.jsonl"
        p.write_text("".join(json.dumps(x) + "\n" for x in self.good))
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            self.assertEqual(differ.main([str(p), str(p), "--accept", str(self.dir / "s.txt")]), 2)
        for name, fn in {
            "live_handles": lambda L: L[2].update(live_handles=2),
            "free_records": lambda L: L[2].update(free_records=100),
        }.items():
            with self.subTest(name):
                broken = self.mutated(fn)
                self.assertEqual(self.run_accept(broken, copy.deepcopy(broken))[0], 2)

    def test_ignore_is_refused_and_empty_script_is_malformed(self):
        self.assertEqual(self.run_accept(self.good, copy.deepcopy(self.good), extra=("--ignore", "vram"))[0], 2)
        (self.dir / "empty.txt").write_text("# nothing\n\n")
        self.assertEqual(self.run_accept(self.good, self.good, script=self.dir / "empty.txt")[0], 2)

    def test_generic_mode_unchanged(self):
        # Generic mode still reports (not rejects) schema-less traces.
        code, _, _ = self.run_accept([], [], extra=())
        self.assertEqual(code, 2)  # accept mode
        out, err = io.StringIO(), io.StringIO()
        pa = self.dir / "g.jsonl"
        pa.write_text("{}\n")
        with redirect_stdout(out), redirect_stderr(err):
            self.assertEqual(differ.main([str(pa), str(pa)]), 0)

def find_binary():
    env = os.environ.get("OKAGE_HANDLE_TRACE")
    if env:
        return Path(env)
    return ROOT / "build" / "handle-trace-release" / "okage-texture-handle-trace"

BINARY = find_binary()

@unittest.skipUnless(BINARY.exists(), f"native tool missing: {BINARY} (set OKAGE_HANDLE_TRACE)")
class NativeTraceTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.dir = Path(self.tmp.name)
        self.count = 0

    def run_script(self, text, expect=0):
        self.count += 1
        script = self.dir / f"s{self.count}.txt"
        trace = self.dir / f"t{self.count}.jsonl"
        script.write_text(text)
        proc = subprocess.run([str(BINARY), str(script), str(trace)], capture_output=True, text=True, timeout=300)
        self.assertEqual(proc.returncode, expect, proc.stderr)
        if expect != 0:
            return proc, None, trace
        return proc, [json.loads(line) for line in trace.read_text().splitlines()], trace

    def test_init_line(self):
        _, lines, _ = self.run_script("")
        self.assertEqual(len(lines), 1)
        init = lines[0]
        self.assertEqual((init["op"], init["text"], init["result"]), (-1, "init", None))
        self.assertEqual(init["handles"], [])
        self.assertEqual(init["records"], [])
        self.assertEqual(init["free_records"], 512)
        self.assertEqual(init["free_extras"], 128)
        self.assertEqual(init["vram"]["available"], 4096)
        self.assertEqual(init["vram"]["touched"], 0)
        self.assertEqual(init["vram"]["touched_sha256"], hashlib.sha256(b"").hexdigest())
        self.assertEqual(len(init["vram"]["cursors"]), 64)
        self.assertEqual(len(init["vram"]["occupancy_sha256"]), 64)

    def test_determinism(self):
        script = ("# comment\n\ncache 1\nlower 1\nreserve 4\nimage 19 32 32\nxim 19 32 32 3 16\nname h0 Foo.XIM\n"
                  "lookup foo.xim\npalette 16\nbind h0 2 p0\nbind h0 1 null\nfiller 1 0 64 64\nretain f0 2\nfree f0\n"
                  "xmd A B failx C\nretire h1\n")
        _, _, first = self.run_script(script)
        _, _, second = self.run_script(script)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertTrue(first.read_bytes().endswith(b"\n"))

    def test_image_then_retire_frees_descriptor(self):
        _, lines, _ = self.run_script("image 19 32 32\nretire h0\n")
        made, retired = lines[1], lines[2]
        self.assertEqual(made["result"], 0)
        self.assertEqual(made["handles"], [[0, 0]])
        self.assertEqual(made["descriptors"], [[0, 1]])
        self.assertEqual(made["records"][0]["image"], 0)
        self.assertEqual(made["records"][0]["pal0"], -1)
        self.assertGreater(made["vram"]["touched"], 0)
        self.assertLess(made["vram"]["available"], lines[0]["vram"]["available"])
        self.assertEqual(retired["handles"], [])
        self.assertEqual(retired["records"], [])
        self.assertEqual(retired["descriptors"], [])
        self.assertEqual(retired["live_handles"], 0)
        self.assertEqual(retired["free_records"], 512)
        self.assertEqual(retired["vram"]["available"], lines[0]["vram"]["available"])
        # VRAM bytes keep their contents after a free.
        self.assertEqual(retired["vram"]["touched_sha256"], made["vram"]["touched_sha256"])

    def test_lookup_after_name_with_cache_creates_alias_without_upload(self):
        _, lines, _ = self.run_script("cache 1\nimage 19 32 32\nname h0 Foo.XIM\nlookup foo.xim\nretire h1\nretire h0\n")
        named, alias = lines[3], lines[4]
        self.assertEqual(named["records"][0]["name"], "foo.xim")
        self.assertEqual(alias["result"], 1)
        self.assertEqual(alias["live_handles"], 2)
        self.assertEqual(alias["records"][0]["handles"], 2)
        self.assertEqual(len(alias["records"]), 1)
        self.assertEqual(alias["descriptors"], named["descriptors"])
        self.assertEqual(alias["vram"], named["vram"])
        self.assertEqual(alias["handles"], [[0, 0], [1, 0]])
        self.assertEqual(lines[5]["records"][0]["handles"], 1)
        self.assertEqual(lines[6]["records"], [])
        self.assertEqual(lines[6]["descriptors"], [])

    def test_lookup_miss_returns_minus_one_but_consumes_label(self):
        _, lines, _ = self.run_script("lookup nothing\nimage 19 32 32\nretire h1\n")
        self.assertEqual(lines[1]["result"], -1)
        self.assertEqual(lines[3]["handles"], [])

    def test_xmd_requests_and_abort(self):
        _, lines, _ = self.run_script("cache 1\nxmd A B\nxmd a b c failz d\n")
        self.assertEqual(lines[2]["file_requests"], ["A.xim", "B.xim"])
        self.assertGreaterEqual(lines[2]["result"], 0)
        # Cache hits for a/b (names are lower-cased by lookup); c and failz are read, d never.
        self.assertEqual(lines[3]["file_requests"], ["c.xim", "failz.xim"])
        self.assertEqual(lines[3]["result"], -1)
        self.assertEqual(lines[3]["live_handles"], lines[2]["live_handles"] + 3)

    def test_palette_filler_free_retain(self):
        _, lines, _ = self.run_script("palette 16\nfiller 1 0 64 64\nretain f0 1\nretain f0 -1\nfree f0\n")
        self.assertIsNotNone(lines[1]["result"])
        self.assertEqual(lines[1]["descriptors"], [[lines[1]["result"], 0]])
        self.assertGreater(lines[1]["vram"]["touched"], 0)
        filler = lines[2]["result"]
        self.assertEqual(lines[2]["descriptors"], sorted(lines[1]["descriptors"] + [[filler, 0]]))
        self.assertEqual([d for d in lines[3]["descriptors"] if d[0] == filler], [[filler, 1]])
        self.assertEqual([d for d in lines[4]["descriptors"] if d[0] == filler], [[filler, 0]])
        self.assertEqual([d for d in lines[5]["descriptors"] if d[0] == filler], [])
        self.assertEqual(lines[5]["handles"], [])

    def test_xim_levels_bind_extras(self):
        _, lines, _ = self.run_script("xim 19 32 32 3 16\nretire h0\n")
        rec = lines[1]["records"][0]
        self.assertNotEqual(rec["pal0"], -1)
        self.assertEqual(len(rec["extra"]), 2)
        self.assertEqual(lines[1]["free_extras"], 126)
        self.assertEqual(len(lines[1]["descriptors"]), 4)
        self.assertEqual(lines[2]["descriptors"], [])
        self.assertEqual(lines[2]["free_extras"], 128)

    def test_reserve(self):
        _, lines, _ = self.run_script("reserve 3\n")
        self.assertEqual(lines[1]["result"], 0)
        self.assertEqual(lines[1]["reserved"], [0, 1, 2])

    def assert_rejected(self, script, fragment):
        proc, _, trace = self.run_script(script, expect=2)
        self.assertIn(fragment, proc.stderr)
        self.assertFalse(trace.exists(), "a rejected script must not leave a trace file")

    def test_strict_script_validation(self):
        self.assert_rejected("frobnicate 1\n", "unknown op 'frobnicate'")
        self.assert_rejected("retire h0\n", "not defined yet")
        self.assert_rejected("image 19 32 32\nretire x0\n", "bad label")
        self.assert_rejected("image 19 32 32\nretire h01\n", "malformed number")
        self.assert_rejected("image 19 32 32\nretire h1\n", "not defined yet")
        self.assert_rejected("image 19 3x 32\n", "malformed number")
        self.assert_rejected("image 19 -3 32\n", "malformed number")
        self.assert_rejected("image 19 0 32\n", "out of range")
        self.assert_rejected("image 19 4096 32\n", "out of range")
        self.assert_rejected("image 99 32 32\n", "PSM 99 unsupported")
        self.assert_rejected("image 256 32 32\n", "out of range")
        self.assert_rejected("filler 1 99 32 32\n", "not an allocator pixel storage format")
        self.assert_rejected("filler 4 0 32 32\n", "out of range")
        self.assert_rejected("cache 2\n", "must be 0 or 1")
        self.assert_rejected("reserve 0\n", "out of range")
        self.assert_rejected("palette 0\n", "out of range")
        self.assert_rejected("image 19 32\n", "takes 3 operand")
        self.assert_rejected("xim 19 32 32 1 17\n", "colors must be 16 or 256")
        self.assert_rejected("image 19 32 32\npalette 16\nbind h0 1 p1\n", "not defined yet")
        self.assert_rejected("filler 1 0 8 8\nretain f0 abc\n", "malformed number")
        self.assert_rejected("filler 1 0 8 8\nretain f0 99999999999\n", "malformed number")

    def test_unsupported_cache_off_after_on(self):
        proc, _, _ = self.run_script("cache 1\ncache 0\n", expect=2)
        self.assertIn("not supported by the native API", proc.stderr)

    def test_refuses_to_overwrite_trace(self):
        script = self.dir / "s.txt"
        trace = self.dir / "t.jsonl"
        script.write_text("reserve 1\n")
        trace.write_text("precious\n")
        proc = subprocess.run([str(BINARY), str(script), str(trace)], capture_output=True, text=True)
        self.assertEqual(proc.returncode, 2)
        self.assertIn("refusing to overwrite", proc.stderr)
        self.assertEqual(trace.read_text(), "precious\n")

    def test_usage_and_missing_script(self):
        self.assertEqual(subprocess.run([str(BINARY)], capture_output=True).returncode, 2)
        proc = subprocess.run([str(BINARY), str(self.dir / "nope.txt"), str(self.dir / "o.jsonl")],
                              capture_output=True, text=True)
        self.assertEqual(proc.returncode, 2)
        self.assertIn("cannot read script", proc.stderr)

    def test_native_trace_against_itself_through_differ(self):
        script = "image 19 32 32\nxim 0 16 16 0 0\nretire h0\n"
        _, _, a = self.run_script(script)
        _, _, b = self.run_script(script)
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(differ.main([str(a), str(b)]), 0)
        _, _, c = self.run_script("image 19 32 32\nxim 0 16 16 0 0\nretire h1\n")
        with redirect_stdout(out):
            self.assertEqual(differ.main([str(a), str(c)]), 1)

if __name__ == "__main__":
    unittest.main()
