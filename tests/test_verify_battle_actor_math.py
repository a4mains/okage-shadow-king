"""Synthetic structure tests; executed original observations are separate evidence."""
import copy
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import verify_battle_actor_math as math_probe

def observed_rows():
    rows = [{'schema': 'okage-battle-construction-v2', 'elf_sha256': math_probe.ELF_SHA256}]
    seed, ordinal, registered = 1056117600, 0, 0
    def append(row):
        row.update(sequence=len(rows)-1, frame=10+len(rows))
        rows.append(row)
    def raw(data, address):
        return dict(raw_hex=data.hex(), bytes=len(data), address=address)
    for cid, argument in enumerate([0, 0x60003, 0, 1, 2], 1):
        party = cid <= 2
        child = bytearray(0x3d0 if party else 0x3e0)
        struct.pack_into('<h', child, 0x16c, -11)
        struct.pack_into('<i', child, 0x378, -12)
        struct.pack_into('<I', child, 0x194+16, 0 if cid == 2 else 0x3f800000)
        # Synthetic captured outputs need not implement the arithmetic.
        struct.pack_into('<II', child, 0x194, 0x3f123456, 0x40123456)
        struct.pack_into('<h', child, 0x398, 4)
        ptr = 0x10000 + cid*0x1000
        entry = dict(event='constructor_entry', constructor_id=cid, kind='party' if party else 'enemy',
                     pc=0x153618 if party else 0x15b580, ra=0x115384, sp=0x8000,
                     constructor_entry_sp=0x8000, argument=argument, child_pointer=ptr,
                     body_pointer=ptr+0x40, rng_entries_since_entry=0, rng_state=seed,
                     child=raw(child, ptr), entry_context=raw(bytes(0x38), 0x4000), source_slot=argument >> 16)
        append(entry)
        # First actor also registers two empty slots, totaling seven.
        for _ in range(3 if cid == 1 else 1):
            registration = dict(constructor_id=cid, registration_index=registered, body_pointer=ptr+0x40,
                                stats_pointer=0x9000, registration_kind=0, battle_root=0xa000,
                                return_pc=0x1537c0 if party else 0x15b7b4, registered_count=registered)
            append(dict(registration, event='registration_entry', pc=0x1512f0))
            registered += 1
            append(dict(registration, event='registration_complete', pc=registration['return_pc'], registered_count=registered))
        ras = [0x15455c] if party else [0x15b794, 0x15c1cc, 0x154588]
        for ra in ras:
            ordinal += 1
            append(dict(event='rng_entry', constructor_id=cid, pc=0x1f14b0, ra=ra,
                        rng_state=seed, rng_entry_ordinal=ordinal))
            seed = math_probe.advance(seed)
        append(dict(entry, event='constructor_complete', pc=0x115384,
                    rng_state=seed, rng_entries_since_entry=len(ras)))
    rows.append(dict(event='finish', rows=len(rows)-1, party_completed=2,
                     enemy_completed=3, registrations_completed=7, rng_entries_observed=ordinal))
    return rows

def output(cases):
    return b''.join(struct.pack('<4I', c['id'], c['op'], c['consumed'], 0)
                    + bytes.fromhex(c['expected_hex']) for c in cases)

class BattleActorMathTests(unittest.TestCase):
    def test_extraction_signed_parameters_and_stan_scope(self):
        cases = math_probe.extract(observed_rows())
        self.assertEqual(len(cases), 8)
        self.assertEqual([c['parameter'] for c in cases if c['op'] == 0], [-11, -11, -12, -12, -12])
        self.assertEqual(struct.unpack_from('<I', bytes.fromhex(cases[1]['expected_hex']), 32)[0], 0x3f800000)
        self.assertIn('overwrites', cases[1]['comparison_note'])
        result = math_probe.compare(cases, math_probe.fixture(cases), output(cases))
        self.assertEqual((result['timer_cases'], result['selector_cases'], result['compared_guarded_bytes']), (5, 3, 1024))
        self.assertIn('No full-constructor', result['scope'])

    def test_observer_identity_extent_sequence_and_completion(self):
        rows = observed_rows()
        changes = [(0, 'schema', 'old'), (0, 'elf_sha256', '0'*64),
                   (1, 'sequence', 1), (1, 'pc', 0), (-1, 'rows', 0),
                   (-1, 'enemy_completed', 2)]
        for index, key, value in changes:
            bad = copy.deepcopy(rows); bad[index][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                math_probe.extract(bad)
        with self.assertRaises(ValueError):
            math_probe.extract(rows[:-1])

    def test_rng_association_interleaving_and_state_changes(self):
        rows = observed_rows()
        index = next(i for i, r in enumerate(rows) if r.get('event') == 'rng_entry')
        for key, value in [('ra', 0x123456), ('constructor_id', 999), ('rng_state', 0), ('pc', 0)]:
            bad = copy.deepcopy(rows); bad[index][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                math_probe.extract(bad)
        end = next(i for i, r in enumerate(rows) if r.get('event') == 'constructor_complete')
        for key, value in [('rng_entries_since_entry', 0), ('rng_state', 0), ('child_pointer', 0), ('source_slot', 6)]:
            bad = copy.deepcopy(rows); bad[end][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                math_probe.extract(bad)

    def test_registration_pair_and_child_raw_extent(self):
        rows = observed_rows()
        for key, value in [('registration_index', 1), ('return_pc', 0), ('registered_count', 1)]:
            bad = copy.deepcopy(rows); bad[2][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                math_probe.extract(bad)
        for key, value in [('bytes', 1), ('raw_hex', ''), ('address', 0)]:
            bad = copy.deepcopy(rows); bad[1]['child'][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                math_probe.extract(bad)

    def test_output_extent_identity_counts_and_all_guarded_bytes(self):
        cases = math_probe.extract(observed_rows())
        inputs, expected = math_probe.fixture(cases), output(cases)
        for invalid in (b'', expected[:-1], expected+b'\0'):
            with self.assertRaisesRegex(ValueError, 'extent'):
                math_probe.compare(cases, inputs, invalid)
        for offset in (0, 4, 8, 12, 16, 32, 44, 143):
            bad = bytearray(expected); bad[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                math_probe.compare(cases, inputs, bad)
        with self.assertRaisesRegex(ValueError, 'Fixture identity'):
            math_probe.compare(cases, inputs[:-1], expected)

    def test_reference_manifest_and_fresh_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            reference, manifest = root/'observer.jsonl', root/'run.json'
            reference.write_text('\n'.join(json.dumps(r) for r in observed_rows()))
            (root/'save').write_bytes(b'save'); (root/'pad').write_bytes(b'pad')
            run = dict(exit_code=0, command=['binary', 'arg', 'arg', 'save', '--pad', 'pad', '--battle-construction', 'observer.jsonl'],
                       binary_sha256='1'*64, source_save_sha256=math_probe.sha(b'save'), input_sha256=math_probe.sha(b'pad'))
            manifest.write_text(json.dumps(run))
            self.assertEqual(len(math_probe.load_reference(reference, manifest, root)), 8)
            for key, value in [('exit_code', 1), ('source_save_sha256', '0'*64), ('input_sha256', '0'*64), ('binary_sha256', '')]:
                manifest.write_text(json.dumps(dict(run, **{key: value})))
                with self.subTest(key=key), self.assertRaises(ValueError):
                    math_probe.load_reference(reference, manifest, root)
            path = root/'new.bin'; math_probe.fresh(path, b'first')
            with self.assertRaises(FileExistsError):
                math_probe.fresh(path, b'second')
            self.assertEqual(path.read_bytes(), b'first')
            link = root/'link'; link.symlink_to(root/'absent')
            with self.assertRaises(FileExistsError):
                math_probe.fresh(link, b'bad')

if __name__ == '__main__':
    unittest.main()
