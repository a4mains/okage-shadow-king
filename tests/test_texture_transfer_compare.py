#!/usr/bin/env python3
"""Controls for okage-texture-transfer-compare and texture_transfer_fixture.py.

Comparator binary: $OKAGE_TRANSFER_COMPARE, else build/handle-cmp-release/
okage-texture-transfer-compare; skipped when missing.
Valid 7-case fixture (retained research evidence, not tracked):
$OKAGE_TRANSFER_FIXTURE, else local/research/upload-original-transfer-v1/fixture.txt
in this checkout or in the primary checkout; those tests skip when absent.
Startup: $OKAGE_TRANSFER_FIXTURE_V2, else local/research/startup-format-transfer-v1/run1/fixture.txt
(122-case PSM20/36/44 + PSM0/19 regression fixture, two complementary-fill original runs).
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import texture_transfer_fixture as gen  # noqa: E402

def find_binary():
    env = os.environ.get("OKAGE_TRANSFER_COMPARE")
    cands = [Path(env)] if env else [ROOT / "build/handle-cmp-release/okage-texture-transfer-compare"]
    return next((c for c in cands if c.is_file()), None)

def find_fixture(env_name="OKAGE_TRANSFER_FIXTURE", rel_path="local/research/upload-original-transfer-v1/fixture.txt"):
    env = os.environ.get(env_name)
    rel = Path(rel_path)
    cands = [Path(env)] if env else [ROOT / rel]
    if not env:
        try:
            common = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--git-common-dir"],
                                    capture_output=True, text=True, check=True).stdout.strip()
            cands.append((ROOT / common).resolve().parent / rel)
        except (OSError, subprocess.CalledProcessError):
            pass
    return next((c for c in cands if c.is_file()), None)

BIN = find_binary()
FIXTURE = find_fixture()
FIXTURE_V2 = find_fixture("OKAGE_TRANSFER_FIXTURE_V2", "local/research/startup-format-transfer-v1/run1/fixture.txt")

class CompareBase(unittest.TestCase):
    def setUp(self):
        if BIN is None:
            self.skipTest("comparator binary not built (set OKAGE_TRANSFER_COMPARE)")
        self.tmp = Path(tempfile.mkdtemp(prefix="handle-compare-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def run_text(self, text, name="f.txt"):
        p = self.tmp / name
        p.write_text(text)
        r = subprocess.run([str(BIN), str(p)], capture_output=True, text=True, timeout=float(os.environ.get("OKAGE_TRANSFER_TIMEOUT", "300")))
        return r.returncode, r.stdout, r.stderr

class ValidFixtureControls(CompareBase):
    def setUp(self):
        super().setUp()
        if FIXTURE is None:
            self.skipTest("retained valid fixture absent (set OKAGE_TRANSFER_FIXTURE)")
        self.lines = FIXTURE.read_text().split("\n")
        self.heads = [i for i, l in enumerate(self.lines) if l.startswith("case ")]
        self.assertEqual(len(self.heads), 7)

    def mutate_header(self, case, field, fn):
        """field index into the header tokens; fn maps int -> int."""
        lines = list(self.lines)
        t = lines[self.heads[case]].split()
        t[field] = str(fn(int(t[field])))
        lines[self.heads[case]] = " ".join(t)
        return "\n".join(lines)

    def expect_diff(self, text, case, needle):
        code, out, _ = self.run_text(text)
        self.assertEqual(code, 1, out)
        diff = [l for l in out.splitlines() if l.startswith("case ") and "DIFF" in l]
        self.assertEqual(len(diff), 1, out)
        self.assertTrue(diff[0].startswith(f"case {case} "), diff[0])
        self.assertIn(needle, diff[0])
        return out

    def test_valid_seven_cases_match(self):
        code, out, _ = self.run_text("\n".join(self.lines))
        self.assertEqual(code, 0, out)
        self.assertEqual(out.count("MATCH"), 7)
        self.assertIn("7 cases, 0 differences", out)

    def test_byte_negative(self):
        lines = list(self.lines)
        i = self.heads[5] + 1
        a, v = lines[i].split()
        lines[i] = f"{a} {int(v) ^ 1}"
        self.expect_diff("\n".join(lines), 5, "mismatching bytes 1")

    def test_byte_negative_address_moved(self):
        # Same shape as the retained fixture-negative.txt, but with an in-range address.
        lines = list(self.lines)
        i = self.heads[5] + 10
        lines[i] = "4000000 " + lines[i].split()[1]
        self.expect_diff("\n".join(lines), 5, "mismatching bytes 2")

    def test_retained_negative_with_out_of_range_address_is_malformed(self):
        lines = list(self.lines)
        i = self.heads[5] + 10
        lines[i] = "9999999 1"
        code, out, err = self.run_text("\n".join(lines))
        self.assertEqual((code, out), (2, ""))
        self.assertIn("address out of range", err)

    # Header columns: case kind psm w h rows lo hi tbp tbw count -> tokens 0..11
    def test_descriptor_width_negative(self):
        self.expect_diff(self.mutate_header(0, 8, lambda hi: hi ^ (1 << 6)), 0, "width original 65 native 64")

    def test_descriptor_height_negative(self):
        self.expect_diff(self.mutate_header(0, 8, lambda hi: hi ^ (1 << 18)), 0, "height original")

    def test_descriptor_psm_negative(self):
        self.expect_diff(self.mutate_header(1, 8, lambda hi: hi ^ 1), 1, "psm original 18 native 19")

    def test_descriptor_id_negative(self):
        self.expect_diff(self.mutate_header(1, 7, lambda lo: lo ^ (1 << 20)), 1, "id original 1 native 0")

    def test_descriptor_tbp_negative(self):
        text = self.mutate_header(0, 7, lambda lo: lo ^ 1)
        text = "\n".join(self._set_col(text, 0, 9, lambda v: v ^ 1))
        self.expect_diff(text, 0, "tbp original 1 native 0")

    def _set_col(self, text, case, field, fn):
        lines = text.split("\n")
        t = lines[self.heads[case]].split()
        t[field] = str(fn(int(t[field])))
        lines[self.heads[case]] = " ".join(t)
        return lines

class MalformedFixtures(CompareBase):

    def head(self, n=0, **kw):
        # lo = tbw 1 << 14, hi = psm0 | w4<<6 | h4<<18
        return f"case 0 image 0 4 4 0 16384 {4 << 6 | 4 << 18} 0 1 {n}\n".replace("case 0", f"case {kw.get('case', 0)}")

    def reject(self, text, needle):
        code, out, err = self.run_text(text)
        self.assertEqual(code, 2, (out, err))
        self.assertEqual(out, "", "malformed input must print no verdicts")
        self.assertIn("fixture error:", err)
        self.assertIn(needle, err)

    def test_missing_file(self):
        r = subprocess.run([str(BIN), str(self.tmp / "nope.txt")], capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)
        self.assertIn("cannot open", r.stderr)

    def test_empty(self):
        self.reject("", "no cases")

    def test_bad_tag(self):
        self.reject("kase 0 image 0 4 4 0 0 0 0 0 0\n", "bad header tag")

    def test_trailing_junk(self):
        self.reject(self.head(1) + "0 1\n" + "junk\n", "bad header tag")

    def test_truncated_header(self):
        self.reject("case 0 image 0 4 4 0 16384\n", "truncated")

    def test_truncated_touched(self):
        self.reject(self.head(3) + "0 1\n1 2\n", "truncated")

    def test_truncated_pair(self):
        self.reject(self.head(1) + "0\n", "truncated")

    def test_bad_kind(self):
        self.reject("case 0 palette 0 4 4 0 16384 0 0 1 0\n", "unknown kind")

    def test_unsupported_psm(self):
        self.reject("case 0 image 20 4 4 0 16384 0 0 1 0\n", "unsupported psm")

    def test_non_numeric(self):
        self.reject("case 0 image 0 x 4 0 16384 0 0 1 0\n", "non-numeric")

    def test_negative_number(self):
        self.reject("case 0 image 0 -4 4 0 16384 0 0 1 0\n", "non-numeric")

    def test_oversize_dimension(self):
        self.reject("case 0 image 0 257 4 0 16384 0 0 1 0\n", "out of range")

    def test_zero_dimension(self):
        self.reject("case 0 image 0 4 0 0 16384 0 0 1 0\n", "zero height")

    def test_huge_integer(self):
        self.reject("case 0 image 0 99999999999999999999999 4 0 16384 0 0 1 0\n", "non-numeric or oversize")

    def test_oversize_count(self):
        self.reject(self.head(4194305), "touched count out of range")

    def test_huge_count_without_data(self):
        self.reject(self.head(4194304), "truncated")

    def test_clut_rows_inconsistent(self):
        self.reject("case 0 clut 0 16 0 4 16384 0 0 1 0\n", "clut rows")

    def test_image_rows_nonzero(self):
        self.reject("case 0 image 0 4 4 2 16384 0 0 1 0\n", "image rows")

    def test_tbp_tbw_disagree_with_lo(self):
        self.reject("case 0 image 0 4 4 0 16384 0 0 2 0\n", "disagree")

    def test_address_out_of_range(self):
        self.reject(self.head(1) + "4194304 1\n", "address out of range")

    def test_value_out_of_range(self):
        self.reject(self.head(1) + "0 256\n", "value out of range")

    def test_duplicate_address(self):
        self.reject(self.head(2) + "7 1\n7 2\n", "duplicate touched address 7")

    def test_error_in_late_case_prints_no_verdict(self):
        # Case 0 is structurally fine; case 1 is truncated. Nothing may be compared.
        self.reject(self.head(1) + "0 1\n" + self.head(5, case=1) + "0 1\n", "truncated")

def a32_row0(x):
    """PSMCT32 byte address of pixel (x, 0), TBP 0 / TBW 1, x < 32 (hand-derivable: x1 +4, x2 +0x10, x4 +0x20,
    x8 -> next block +0x100, x16 -> +0x400)."""
    ix = x % 8
    word = (ix & 1) | ((ix & 2) << 1) | ((ix & 4) << 1)
    return [0, 0x100, 0x400, 0x500][x // 8] + word * 4

def a4_row0(x):
    """PSMT4 (byte, low nibble) of pixel (x, 0), TBP 0 / TBW 1, x < 32: block 0, column 0, sub 0."""
    ix = x % 8
    word = (ix & 1) | ((ix & 2) << 1) | ((ix & 4) << 1)
    return word * 4 + (x >> 3)

def nibble_f0_0f(i):
    """Pixel i of the 'f0/0f' payload (bytes F0, 0F, F0, ...; low nibble is the first pixel)."""
    return (0, 15, 15, 0)[i % 4]

def v2_header(n, psm, w, h, count, x=0, y=0, tbp=0, tbw=1, pattern=3, seed=1, pre_lo=0, pre_len=0):
    lo = tbp | (tbw << 14)
    hi = psm | (w << 6) | (h << 18)
    return f"case2 {n} 2 {psm} {w} {h} {x} {y} {tbp} {tbw} {pattern} {seed} {pre_lo} {pre_len} {lo} {hi} {count}\n"

def small_v2(psm):
    """Hand-derived expectation: a 32x1 transfer of the f0/0f payload (one quadword)."""
    entries = []
    for x in range(32):
        v = nibble_f0_0f(x)
        if psm == 36:
            entries.append((a32_row0(x) + 3, v, 1))
        elif psm == 44:
            entries.append((a32_row0(x) + 3, v << 4, 2))
        else:
            entries.append((a4_row0(x), v, 1))
    return entries

def v2_text(psm, entries):
    return v2_header(0, psm, 32, 1, len(entries)) + "".join(f"{a} {v} {m}\n" for a, v, m in entries)

class FormatMaskControls(CompareBase):
    """Self-contained PSM20/36/44 controls (no retained evidence needed): hand-derived expectations for a
    one-quadword transfer, and negative controls the comparator must report as DIFF with exit code 1."""

    def run_psm(self, psm, entries):
        return self.run_text(v2_text(psm, entries))

    def test_hand_derived_match(self):
        for psm in (20, 36, 44):
            with self.subTest(psm=psm):
                code, out, err = self.run_psm(psm, small_v2(psm))
                self.assertEqual(code, 0, (out, err))
                self.assertIn("1 cases, 0 differences", out)

    def expect_diff(self, psm, entries, needle):
        code, out, _ = self.run_psm(psm, entries)
        self.assertEqual(code, 1, out)
        self.assertIn("DIFF", out)
        self.assertIn(needle, out)

    def test_nibble_flipped(self):
        for psm in (20, 36, 44):
            with self.subTest(psm=psm):
                e = small_v2(psm)
                a, v, m = e[1]  # a nonzero pixel
                e[1] = (a, v ^ (1 if m == 1 else 0x10), m)
                self.expect_diff(psm, e, "written values differ in 1 bytes")

    def test_mask_bit_dropped(self):
        for psm in (20, 36, 44):
            with self.subTest(psm=psm):
                e = small_v2(psm)
                del e[5]
                self.expect_diff(psm, e, "write-mask differs")

    def test_mask_bit_added(self):
        for psm in (36, 44):
            with self.subTest(psm=psm):
                e = small_v2(psm)
                a, v, m = e[0]
                e[0] = (a, v | (0xF0 if m == 1 else 0x0F), 3)  # claims the other nibble was written too
                self.expect_diff(psm, e, "write-mask differs")

    def test_wrong_lane(self):
        # HL data attributed to the HH lane (and vice versa) is a mask difference, not a value difference.
        for psm, shift, mask in ((36, 4, 2), (44, -4, 1)):
            with self.subTest(psm=psm):
                e = small_v2(psm)
                a, v, m = e[1]
                e[1] = (a, v << 4 if shift > 0 else v >> 4, mask)
                self.expect_diff(psm, e, "write-mask differs in 1 bytes")

    def test_wrong_address_lane_same_nibble(self):
        e = small_v2(36)
        a, v, m = e[1]
        e[1] = (a - 1, v, m)
        self.expect_diff(36, e, "write-mask differs")

    def test_unsupported_psm_fixture_is_malformed(self):
        for psm in (1, 27, 2, 18):
            with self.subTest(psm=psm):
                code, out, err = self.run_text(v2_header(0, psm, 32, 1, 0))
                self.assertEqual((code, out), (2, ""))
                self.assertIn("unsupported psm", err)

    def test_malformed_v2(self):
        bad = {
            "empty mask": v2_header(0, 36, 32, 1, 1) + "3 5 0\n",
            "value in unwritten nibble": v2_header(0, 36, 32, 1, 1) + "3 16 1\n",
            "duplicate": v2_header(0, 36, 32, 1, 2) + "3 5 1\n3 6 1\n",
            "no quadword": v2_header(0, 36, 3, 3, 0),
            "truncated": v2_header(0, 36, 32, 1, 2) + "3 5 1\n",
            "window outside vram": v2_header(0, 36, 32, 1, 0, pre_lo=4194304, pre_len=1),
            "bad descriptor": v2_header(0, 36, 32, 1, 0).replace(" 16384 ", " 0 ", 1),
        }
        for name, text in bad.items():
            with self.subTest(name=name):
                code, out, err = self.run_text(text)
                self.assertEqual((code, out), (2, ""), (name, err))
                self.assertIn("fixture error:", err)

    def test_native_rejection_is_diff_not_crash(self):
        # x origin 2047 + 32 pixels leaves the guarded coordinate range: native throws, original did not.
        text = v2_header(0, 36, 32, 1, 0, x=2040)
        code, out, _ = self.run_text(text)
        self.assertEqual(code, 1, out)
        self.assertIn("native fresh run threw", out)

class RetainedV2Controls(CompareBase):
    """Controls against the retained 122-case original-execution fixture (skipped when absent)."""

    def setUp(self):
        super().setUp()
        if FIXTURE_V2 is None:
            self.skipTest("retained v2 fixture absent (set OKAGE_TRANSFER_FIXTURE_V2)")
        self.lines = FIXTURE_V2.read_text().split("\n")
        self.heads = [i for i, l in enumerate(self.lines) if l.startswith("case2 ")]

    def head_for(self, psm, kind="2", pre=False):
        for i in self.heads:
            t = self.lines[i].split()
            if t[3] == str(psm) and t[2] == kind and (int(t[13]) > 0) == pre and int(t[16]) > 8:
                return i
        self.fail("no suitable case")

    def one_diff(self, lines, head, needle):
        code, out, _ = self.run_text("\n".join(lines))
        self.assertEqual(code, 1, out)
        diff = [l for l in out.splitlines() if "DIFF" in l]
        self.assertEqual(len(diff), 1, out)
        self.assertTrue(diff[0].startswith(f"case2 {self.lines[head].split()[1]} "), diff[0])
        self.assertIn(needle, diff[0])

    def test_valid_all_match(self):
        code, out, _ = self.run_text("\n".join(self.lines))
        self.assertEqual(code, 0, out)
        self.assertEqual(out.count("MATCH"), len(self.heads))
        self.assertIn(f"{len(self.heads)} cases, 0 differences", out)

    def test_nibble_flip_each_format(self):
        for psm in (20, 36, 44):
            with self.subTest(psm=psm):
                h = self.head_for(psm)
                lines = list(self.lines)
                a, v, m = map(int, lines[h + 3].split())
                lines[h + 3] = f"{a} {v ^ (0x10 if m & 2 else 1)} {m}"
                self.one_diff(lines, h, "written values differ")

    def test_mask_bit_flip_psm36_44(self):
        for psm in (36, 44):
            with self.subTest(psm=psm):
                h = self.head_for(psm)
                lines = list(self.lines)
                a, v, m = map(int, lines[h + 2].split())
                lines[h + 2] = f"{a} {v | (0xF0 if m == 1 else 0x0F)} 3"
                self.one_diff(lines, h, "write-mask differs")

    def test_wrong_lane_psm36(self):
        h = self.head_for(36)
        lines = list(self.lines)
        a, v, m = map(int, lines[h + 2].split())
        self.assertEqual(m, 1)
        lines[h + 2] = f"{a} {v << 4} 2"
        self.one_diff(lines, h, "write-mask differs")

    def test_preload_case_value_negative(self):
        # A case with a pre-existing window: corrupt one written nibble; the fresh-run value check catches it.
        h = self.head_for(36, pre=True)
        lines = list(self.lines)
        a, v, m = map(int, lines[h + 2].split())
        lines[h + 2] = f"{a} {v ^ 1} {m}"
        self.one_diff(lines, h, "written values differ")

class FixtureGenerator(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="handle-gen-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def doc(self, **over):
        case = {"case": 0, "kind": "image", "psm": 0, "w_or_n": 4, "h": 4, "clut_alloc_rows": 0,
                "descriptor": [16384, (4 << 6) | (4 << 18)], "tbp": 0, "tbw": 1, "touched": [[0, 1], [1, 2]]}
        case.update(over)
        return {"cases": [case]}

    def run_gen(self, doc, out="out.txt", raw=None):
        src = self.tmp / "in.json"
        src.write_text(raw if raw is not None else json.dumps(doc))
        r = subprocess.run([sys.executable, str(ROOT / "tools/texture_transfer_fixture.py"), str(src),
                            str(self.tmp / out)], capture_output=True, text=True)
        return r

    def test_valid(self):
        r = self.run_gen(self.doc())
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual((self.tmp / "out.txt").read_text().split("\n")[0],
                         "case 0 image 0 4 4 0 16384 1048832 0 1 2")
        self.assertEqual([p.name for p in self.tmp.iterdir() if p.name.startswith(".fixture-")], [])

    def test_refuses_overwrite(self):
        (self.tmp / "out.txt").write_text("keep")
        r = self.run_gen(self.doc())
        self.assertEqual(r.returncode, 1)
        self.assertIn("refusing to overwrite", r.stderr)
        self.assertEqual((self.tmp / "out.txt").read_text(), "keep")
        self.assertEqual([p.name for p in self.tmp.iterdir() if p.name.startswith(".fixture-")], [])

    def test_malformed_json(self):
        r = self.run_gen(None, raw="{not json")
        self.assertEqual(r.returncode, 1)
        self.assertIn("malformed JSON", r.stderr)
        self.assertFalse((self.tmp / "out.txt").exists())

    def test_rejections(self):
        bad = [
            {"kind": "palette"}, {"psm": 20}, {"w_or_n": 0}, {"w_or_n": 257}, {"h": True},
            {"descriptor": [1]}, {"descriptor": [0, 2 ** 32]}, {"touched": [[0, 256]]},
            {"touched": [[4194304, 1]]}, {"touched": [[5, 1], [5, 2]]}, {"touched": [[0]]},
            {"tbw": 2}, {"clut_alloc_rows": 2},
        ]
        for over in bad:
            with self.subTest(over=over):
                r = self.run_gen(self.doc(**over), out="bad.txt")
                self.assertEqual(r.returncode, 1, r.stdout)
                self.assertFalse((self.tmp / "bad.txt").exists())

    def test_missing_key(self):
        d = self.doc()
        del d["cases"][0]["tbp"]
        r = self.run_gen(d)
        self.assertEqual(r.returncode, 1)
        self.assertIn("missing key 'tbp'", r.stderr)

    def test_no_cases(self):
        self.assertEqual(self.run_gen({"cases": []}).returncode, 1)
        self.assertEqual(self.run_gen({}).returncode, 1)

    def test_clut(self):
        r = self.run_gen(self.doc(kind="clut", w_or_n=16, h=0, clut_alloc_rows=2))
        self.assertEqual(r.returncode, 0, r.stderr)

    # ---- observer version 2 (Startup) ----
    def doc2(self, **over):
        case = {"case": 0, "kind": "descriptor", "psm": 36, "w": 32, "h": 1, "x": 0, "y": 0, "tbp": 0, "tbw": 1,
                "pattern": 3, "seed": 1, "pre_lo": 0, "pre_len": 0, "descriptor": [16384, 36 | (32 << 6) | (1 << 18)],
                "anomalies": 0, "written_count": 2, "written": [[3, 0, 1], [7, 15, 1]]}
        top = {"schema": "okage-texture-transfer-v2", "fills": [0, 255], "code_tables_preserved": True}
        for k in [k for k in over if k.startswith("top_")]:
            top[k[4:]] = over.pop(k)
        case.update(over)
        top["cases"] = [case]
        return top

    def test_v2_valid(self):
        r = self.run_gen(self.doc2())
        self.assertEqual(r.returncode, 0, r.stderr)
        lines = (self.tmp / "out.txt").read_text().split("\n")
        self.assertEqual(lines[0], f"case2 0 2 36 32 1 0 0 0 1 3 1 0 0 16384 {36 | (32 << 6) | (1 << 18)} 2")
        self.assertEqual(lines[1:3], ["3 0 1", "7 15 1"])

    def test_v2_rejections(self):
        bad = [
            {"anomalies": 1}, {"psm": 1}, {"psm": 27}, {"kind": "image"}, {"w": 513}, {"pattern": 4},
            {"top_fills": [165, 90]}, {"top_code_tables_preserved": False}, {"written_count": 3},
            {"written": [[3, 0, 0], [7, 15, 1]]}, {"written": [[3, 16, 1], [7, 15, 1]]},
            {"written": [[3, 0, 1], [3, 1, 1]]}, {"written": [[4194304, 0, 1], [7, 15, 1]]},
            {"pre_lo": 4194304, "pre_len": 1}, {"descriptor": [0, 36 | (32 << 6) | (1 << 18)]},
        ]
        for over in bad:
            with self.subTest(over=over):
                r = self.run_gen(self.doc2(**over), out="bad.txt")
                self.assertEqual(r.returncode, 1, r.stdout)
                self.assertFalse((self.tmp / "bad.txt").exists())

if __name__ == "__main__":
    unittest.main()
