import hashlib
import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import retire_input_provenance as P  # noqa: E402

def sha(b):
    return hashlib.sha256(b).hexdigest()

class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = os.path.realpath(self.tmp.name)
        self.root = root
        self.stage = os.path.join(root, "stage")
        self.ext = os.path.join(root, "ext")
        os.makedirs(os.path.join(self.stage, "sub"))
        os.makedirs(self.ext)
        self.data = {"a.bin": b"alpha", "sub/b.bin": b"bravo" * 100000}
        for rel, b in self.data.items():
            with open(os.path.join(self.stage, rel), "wb") as f:
                f.write(b)
        with open(os.path.join(self.ext, "t.bin"), "wb") as f:
            f.write(b"linked")
        os.symlink(os.path.join(self.ext, "t.bin"), os.path.join(self.stage, "link.bin"))
        self.data["link.bin"] = b"linked"
        self.write_manifest()

    def write_manifest(self):
        files = [{"path": r, "sha256": sha(b)} for r, b in sorted(self.data.items())]
        m = os.path.join(self.stage, P.MANIFEST_NAME)
        with open(m, "w") as f:
            json.dump({"file_count": len(files), "files": files}, f)
        self.msha = P.hash_file(m)[0]

    def run_cmd(self, *argv):
        out = os.path.join(self.root, "out.json")
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = P.main(list(argv) + ["--out", out])
        with open(out) as f:
            return rc, json.load(f)

    def audit(self, msha=None):
        return self.run_cmd("audit", self.stage, "--manifest-sha256", msha or self.msha)

    def log(self, recs, raw=""):
        p = os.path.join(self.root, "reads.jsonl")
        with open(p, "w") as f:
            for r in recs:
                f.write(json.dumps(r) + "\n")
            f.write(raw)
        return p

    def rec(self, rel, **kw):
        b = self.data[rel]
        d = {"path": os.path.join(self.stage, rel),
             "resolved": os.path.realpath(os.path.join(self.stage, rel)),
             "bytes": len(b), "sha256": sha(b)}
        d.update(kw)
        return d

    def reads(self, p, *extra):
        return self.run_cmd("check-reads", self.stage, p, "--manifest-sha256", self.msha, *extra)

class AuditTests(Base):
    def test_clean(self):
        rc, s = self.audit()
        self.assertEqual(rc, 0)
        self.assertEqual(s["kind"], "current-time-audit")
        self.assertIn("audited_at", s)
        self.assertEqual(s["listed_files"], 3)
        self.assertEqual(s["total_bytes"], sum(map(len, self.data.values())))
        self.assertEqual(s["symlink_targets"], {"link.bin": os.path.join(self.ext, "t.bin")})
        self.assertEqual(s["manifest_sha256"], self.msha)

    def test_mismatch(self):
        with open(os.path.join(self.stage, "a.bin"), "wb") as f:
            f.write(b"changed")
        rc, s = self.audit()
        self.assertEqual(rc, 1)
        self.assertEqual(s["mismatched"], 1)

    def test_symlink_target_changed(self):
        with open(os.path.join(self.ext, "t.bin"), "wb") as f:
            f.write(b"other")
        rc, s = self.audit()
        self.assertEqual(rc, 1)
        self.assertEqual(s["failures"][0]["path"], "link.bin")

    def test_missing(self):
        os.remove(os.path.join(self.stage, "a.bin"))
        rc, s = self.audit()
        self.assertEqual((rc, s["missing"]), (1, 1))

    def test_dangling_symlink_is_missing(self):
        os.remove(os.path.join(self.ext, "t.bin"))
        rc, s = self.audit()
        self.assertEqual((rc, s["missing"]), (1, 1))

    def test_unlisted(self):
        with open(os.path.join(self.stage, "sub", "extra.bin"), "wb") as f:
            f.write(b"x")
        rc, s = self.audit()
        self.assertEqual(rc, 1)
        self.assertEqual(s["unlisted_files"], ["sub/extra.bin"])

    def test_wrong_manifest_hash(self):
        rc, s = self.audit("0" * 64)
        self.assertEqual(rc, 1)
        self.assertEqual(s["failures"][0]["type"], "manifest")

class ReadTests(Base):
    def test_ok_and_consumed_assets(self):
        p = self.log([self.rec("sub/b.bin"), self.rec("a.bin"), self.rec("a.bin"), self.rec("link.bin")])
        rc, s = self.reads(p)
        self.assertEqual(rc, 0, s["failures"])
        self.assertEqual(s["kind"], "run-time-read-provenance")
        self.assertEqual((s["reads"], s["distinct_files"]), (4, 3))
        self.assertEqual([a["path"] for a in s["consumed_assets"]], ["a.bin", "link.bin", "sub/b.bin"])
        self.assertEqual(s["current_rehash"]["still_match"], 3)

    def test_non_stage_not_failure_unless_strict(self):
        r = {"path": os.path.join(self.root, "save", "s.dat"), "resolved": "x", "bytes": 1,
             "sha256": sha(b"z")}
        p = self.log([self.rec("a.bin"), r])
        rc, s = self.reads(p)
        self.assertEqual(rc, 0)
        self.assertEqual(len(s["non_stage"]), 1)
        rc, s = self.reads(p, "--strict-stage")
        self.assertEqual(rc, 1)

    def test_dotdot_escape_is_non_stage(self):
        r = self.rec("a.bin", path=self.stage + "/../ext/t.bin")
        rc, s = self.reads(self.log([r]))
        self.assertEqual(len(s["non_stage"]), 1)

    def test_wrong_hash(self):
        rc, s = self.reads(self.log([self.rec("a.bin", sha256=sha(b"nope"))]))
        self.assertEqual(rc, 1)
        self.assertEqual(s["failures"][0]["type"], "sha-mismatch")

    def test_wrong_bytes(self):
        rc, s = self.reads(self.log([self.rec("a.bin", bytes=2)]))
        self.assertEqual(rc, 1)
        self.assertEqual(s["failures"][0]["type"], "size-mismatch")

    def test_not_in_manifest(self):
        with open(os.path.join(self.stage, "new.bin"), "wb") as f:
            f.write(b"n")
        r = {"path": os.path.join(self.stage, "new.bin"), "bytes": 1, "sha256": sha(b"n")}
        rc, s = self.reads(self.log([r]))
        self.assertEqual((rc, s["failures"][0]["type"]), (1, "not-in-manifest"))

    def test_malformed_line(self):
        rc, s = self.reads(self.log([self.rec("a.bin")], raw="{not json\n"))
        self.assertEqual(rc, 1)
        self.assertEqual(s["failures"][0]["type"], "malformed-line")

    def test_missing_field(self):
        rc, s = self.reads(self.log([], raw='{"path": "x"}\n'))
        self.assertEqual(rc, 1)

    def test_empty_log(self):
        rc, s = self.reads(self.log([]))
        self.assertEqual(rc, 1)
        self.assertEqual(s["failures"][0]["type"], "empty-log")

    def test_wrong_manifest_hash(self):
        p = self.log([self.rec("a.bin")])
        self.msha = "1" * 64
        rc, s = self.reads(p)
        self.assertEqual((rc, s["failures"][0]["type"]), (1, "manifest"))

    def test_runtime_ok_but_changed_since(self):
        p = self.log([self.rec("a.bin")])
        with open(os.path.join(self.stage, "a.bin"), "wb") as f:
            f.write(b"ALPHA")
        rc, s = self.reads(p)
        self.assertEqual(rc, 0)  # run-time record matched the manifest
        self.assertEqual(len(s["current_rehash"]["differ"]), 1)

if __name__ == "__main__":
    unittest.main()
