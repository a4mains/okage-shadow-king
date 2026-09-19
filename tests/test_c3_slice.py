"""c3_slice run-evidence reading: scene activations, encounters, services,
status message changes (not the persistent field), trace rows and guards."""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import c3_slice  # noqa: E402

def status(frame, scene, message):
    return json.dumps({'frame': frame, 'scene': scene, 'message': message, 'event_running': False})

class ReadRunTests(unittest.TestCase):
    def test_reads_evidence(self):
        with tempfile.TemporaryDirectory() as d:
            run = Path(d)
            (run / 'stdout.log').write_text('\n'.join([
                status(1, 74, 10110),
                'Native ordinary field activated scene=1 entrance=1 frame=20 pc=31436',
                status(21, 1, 10110),                       # persisted value: not a new message
                status(30, 1, 21020),                       # changed: new message in scene 1
                'Native natural encounter frame=40 row=1 column=5 formation=4 phase=11 rear=0',
                'Native battle stage has no collision member stage=1',
                'Native victory return queued scene=1 frame=90',
                'Native field Rest recovered party frame=95',
                'Native field Save constructed frame=96',
                'Native field Save retired frame=97',
                'Battle music variant ownership is unrecovered: 1',
                '{"guard_pending_opcode":0}',
            ]) + '\n')
            (run / 'event-trace.jsonl').write_text('{"schema":"x"}\n{"frame":5,"scene":14,"script_fnv1a":7,'
                                                     '"script_bytes":9,"pc":40,"op":76}\n')
            (run / 'manifest.json').write_text(json.dumps({'event_trace': True}))
            ev = c3_slice.read_run(run)
        self.assertEqual(list(ev['scenes']), [1])
        self.assertEqual(dict(ev['status_messages']), {1: {21020}})
        self.assertEqual(ev['encounters'], [{'frame': 40, 'formation': 4, 'phase': 11, 'rear': 0}])
        self.assertEqual((ev['stages'], ev['victories'], ev['services']['rest']), ([1], [90], [95]))
        self.assertEqual((ev['services']['save_menu'], ev['services']['save_written']), ([96], [97]))
        self.assertEqual(ev['guards'], ['Battle music variant ownership is unrecovered: 1'])
        self.assertEqual(ev['trace'][0]['op'], 76)
        self.assertTrue(ev['event_trace'])

    def test_fnv1a_matches_the_native_hash(self):
        # 32-bit FNV-1a reference vectors.
        self.assertEqual(c3_slice.fnv1a(b''), 0x811c9dc5)
        self.assertEqual(c3_slice.fnv1a(b'a'), 0xe40c292c)

if __name__ == '__main__':
    unittest.main()
