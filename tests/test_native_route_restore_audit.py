import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from native_route import restore_audit as ra  # noqa: E402

ENTRANCES = [((3.0, -0.001, -10.75), (0.0, 0.48, 0.0)), ((0.04, -0.001, 2.2), (0.0, 0.93, 0.0))]

def stg(entrances=ENTRANCES):
    start = 32
    b = bytearray(start + 28 * len(entrances))
    struct.pack_into('<I', b, 0, 0x475453)
    struct.pack_into('<II', b, 24, start, len(entrances))
    for i, (pos, rot) in enumerate(entrances):
        struct.pack_into('<I6f', b, start + 28 * i, 0, *pos, *rot)
    return bytes(b)

def xdb(scene=74, directory='room/house', room='home2b', music=7):
    strings = b''
    def s(text):
        nonlocal strings
        at = len(strings)
        strings += text.encode() + b'\0'
        return at
    name = s(f'scene{scene}')
    rec = bytearray(13 * 4)
    struct.pack_into('<I', rec, 16, s(directory))
    struct.pack_into('<I', rec, 20, s(room))
    struct.pack_into('<i', rec, 40, music)
    count = 1
    header = bytearray(32 + 16 * count)
    struct.pack_into('<IIIII', header, 0, 0x424458, len(rec), len(strings), count, 0xffffffff)
    struct.pack_into('<II', header, 32, name, 0)
    struct.pack_into('<I', header, 40, 13)
    return bytes(header) + bytes(rec) + strings

def save(scene=74, entrance=1):
    payload = bytearray(0xd78)
    struct.pack_into('<ii', payload, ra.SCENE_WORD, scene, entrance)
    return b'!crc' + struct.pack('<II', 0xd78, 0) + bytes(payload)

def hero(pos, rot):
    words = [hex(struct.unpack('<I', struct.pack('<f', v))[0]) for v in (*rot, 1.0, *pos, 1.0)]
    return {'actors': [{'alias': -3, 'raw_channel_words': words}, {'alias': -1, 'raw_channel_words': words}]}

def report_checkpoint(d, scene, entrance, sha='aa', handles=(5,), alias_offset=0):
    d.mkdir(parents=True)
    (d / 'field.json').write_text(json.dumps({'scene': scene, 'entrance': entrance}))
    hs = [h + alias_offset for h in handles]
    (d / 'world.json').write_text(json.dumps({'models': [{'name': 'wal.xmd', 'handles': hs}], 'xta_scene': hs}))
    (d / 'model-groups.json').write_text(json.dumps({'groups': [{'name': 'g'}], 'actor_registrations': [{'name': 'hero'}]}))
    base = 100 + alias_offset * 10
    (d / 'state.json').write_text(json.dumps({
        'records': [{'record': 1, 'name': '6wal01.xim'}],
        'handles': [{'handle': h, 'record': 1, 'image': [base, base + 1], 'palettes': [[base + 1, base + 2], None]} for h in hs]}))
    rows = [{'kind': 'alloc', 'alloc': base}, {'kind': 'upload-bytes', 'alloc': base, 'bytes': 4, 'sha256': sha},
            {'kind': 'upload-bytes', 'alloc': base + 1, 'bytes': 4, 'sha256': 'cc'}]
    (d / 'timeline.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in rows))

class RestoreAuditTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        t = Path(self.tmp.name)
        self.stage = t / 'stage'
        (self.stage / 'FLDDATA' / 'ROOM' / 'HOUSE' / 'HOME2B.XPF').mkdir(parents=True)
        (self.stage / 'FLDDATA' / 'FLDSET.XDB').write_bytes(xdb())
        (self.stage / 'FLDDATA' / 'ROOM' / 'HOUSE' / 'HOME2B.XPF' / 'a.stg').write_bytes(stg())
        self.run_dir = t / 'run'
        (self.run_dir / 'control').mkdir(parents=True)
        (self.run_dir / 'stdout.log').write_text(
            'Native Restore requested saved field frame=1491\n'
            'Native ordinary field queued scene=74 entrance=1 frame=1494\n'
            'Native ordinary field activated scene=74 entrance=1 frame=1507 pc=432\n')
        self.pose(*ENTRANCES[1])
        self.save = t / 'bkmo2.dat'
        self.save.write_bytes(save())
        self.report = t / 'report'

    def pose(self, pos, rot):
        (self.run_dir / 'control' / 'restored-field.png.actors-state.json').write_text(json.dumps(hero(pos, rot)))

    def test_field_and_pose_pass(self):
        r = ra.audit(self.run_dir, self.stage, self.save)
        self.assertTrue(r['pass'], r)
        self.assertEqual(r['checks']['field']['activated'], (74, 1))
        self.assertEqual(r['checks']['pose']['room'], 'home2b')

    def test_wrong_entrance_pose_fails(self):
        self.pose(*ENTRANCES[0])
        self.assertFalse(ra.audit(self.run_dir, self.stage, self.save)['checks']['pose']['pass'])

    def test_loader_not_matching_the_saved_field_fails(self):
        self.save.write_bytes(save(entrance=0))
        r = ra.audit(self.run_dir, self.stage, self.save)
        self.assertFalse(r['checks']['field']['pass'])

    def test_bindings_equal_across_restore_and_ordinary_load(self):
        report_checkpoint(self.report / 'field-activation-return', 74, 1)
        report_checkpoint(self.report / 'field-2' / 'field-activation-return', 73, 2, sha='zz')
        report_checkpoint(self.report / 'field-3' / 'field-activation-return', 74, 0, alias_offset=7)
        r = ra.audit(self.run_dir, self.stage, self.save, self.report)
        self.assertTrue(r['checks']['bindings']['pass'], r['checks']['bindings'])
        self.assertEqual((r['checks']['bindings']['restored_cycle'], r['checks']['bindings']['ordinary_cycle']), (1, 3))

    def test_changed_content_fails(self):
        report_checkpoint(self.report / 'field-activation-return', 74, 1)
        report_checkpoint(self.report / 'field-3' / 'field-activation-return', 74, 0, sha='bb')
        r = ra.audit(self.run_dir, self.stage, self.save, self.report)
        self.assertFalse(r['checks']['bindings']['pass'])
        self.assertEqual(r['checks']['bindings']['differences'][0]['what'], 'models')

    def test_missing_ordinary_load_fails(self):
        report_checkpoint(self.report / 'field-activation-return', 74, 1)
        r = ra.audit(self.run_dir, self.stage, self.save, self.report)
        self.assertFalse(r['checks']['bindings']['pass'])

    def test_handle_without_uploaded_content_fails(self):
        report_checkpoint(self.report / 'field-activation-return', 74, 1, sha=None)
        report_checkpoint(self.report / 'field-3' / 'field-activation-return', 74, 0, sha=None)
        for cp in (self.report / 'field-activation-return', self.report / 'field-3' / 'field-activation-return'):
            rows = [json.loads(l) for l in (cp / 'timeline.jsonl').read_text().splitlines()]
            rows = [r for r in rows if not (r['kind'] == 'upload-bytes' and r['sha256'] is None)]
            (cp / 'timeline.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in rows))
        r = ra.audit(self.run_dir, self.stage, self.save, self.report)
        self.assertFalse(r['checks']['bindings']['pass'])
        self.assertIn('without uploaded content', r['checks']['bindings']['differences'][-1]['what'])

    def test_field_cycle_directories_only(self):
        report_checkpoint(self.report / 'field-activation-return', 74, 1)
        (self.report / 'field-map-loaded').mkdir()
        self.assertEqual([c for c, _, _, _ in ra.report_loads(self.report)], [1])

if __name__ == '__main__':
    unittest.main()
