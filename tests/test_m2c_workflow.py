"""Executable refusal controls using synthetic ELF and real subprocess/git tools."""
import copy
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from argparse import Namespace

SPEC = importlib.util.spec_from_file_location('m2c_workflow', Path(__file__).resolve().parents[1] / 'tools/m2c_workflow.py')
w = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(w)

def command(cwd, *args):
    return subprocess.check_output(args, cwd=cwd, stderr=subprocess.STDOUT)

def init_repo(path):
    path.mkdir(parents=True)
    command(path, 'git', 'init', '-q')
    command(path, 'git', 'config', 'user.email', 'workflow-test@example.invalid')
    command(path, 'git', 'config', 'user.name', 'Workflow fixture')

def commit(path):
    command(path, 'git', 'add', '.')
    command(path, 'git', 'commit', '-qm', 'synthetic fixture')
    return command(path, 'git', 'rev-parse', 'HEAD').decode().strip()

def make_elf():
    data = bytearray(0x10c)
    data[:7] = b'\x7fELF\x01\x01\x01'
    struct.pack_into('<H', data, 18, 8)
    struct.pack_into('<I', data, 28, 52)
    struct.pack_into('<HH', data, 42, 32, 1)
    struct.pack_into('<8I', data, 52, 1, 0x100, 0x100000, 0x100000, 12, 12, 5, 4)
    data[0x100:0x108] = bytes.fromhex('0800e00300000000')
    struct.pack_into('<I', data, 0x108, 0x100000)
    return bytes(data)

POLICY_SOURCE = '''
class MIPS:
    instrs_ignore = {"nop", "ctc1"}
    instrs_destination_first = {"sll": None}
    def parse(self, mnemonic):
        if mnemonic == "jr": pass
        elif mnemonic in ("mfc0", "mtc0"): pass
'''

class WorkflowTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.repo = self.base / 'repo'
        init_repo(self.repo)
        (self.repo / '.gitignore').write_text('/local/\n')
        commit(self.repo)
        self.data = make_elf()
        self.identity = {'sha256': w.digest(self.data), 'size': len(self.data), 'machine': 8}
        self.elf = w.ELF(self.data, self.identity)
        self.case = {'id': '00100000', 'address': '0x100000', 'end_exclusive': '0x100008', 'size': 8,
                     'elf_offset': '0x100', 'bytes_sha256': w.digest(self.data[0x100:0x108]),
                     'terminal': 'jr_ra_delay_slot', 'rationale': 'synthetic test', 'required_tables': [],
                     'ghidra_provenance': {'map_row': {'address': '00100000', 'span_bytes': '8', 'body_bytes': '8'}}}
        self.config = {'schema_version': 1, 'elf': self.identity, 'target': 'mipsee-gcc-c',
                       'context': 'typedef unsigned int u32;\n', 'dependencies': {}, 'symbols': [],
                       'tables': {}, 'cases': [self.case], 'tools': {}}

    def fake_tools(self, mutate=False):
        m2c, spim = self.base / 'm2c', self.base / 'spim'
        for path in (m2c, spim):
            init_repo(path)
        (m2c / 'm2c').mkdir()
        (m2c / 'm2c/arch_mips.py').write_text(POLICY_SOURCE)
        (m2c / 'm2c.py').write_text('print("unsigned int func_00100000(void) { return 0; }")\n')
        package = spim / 'spimdisasm'
        package.mkdir()
        (package / '__init__.py').write_text('')
        (package / '__main__.py').write_text('''
import sys
from pathlib import Path
args = sys.argv
elf = Path(args[2]).read_bytes()
out = Path(args[3]); out.mkdir(parents=True)
start = int(args[args.index('--start')+1], 0)
end = int(args[args.index('--end')+1], 0)
vram = int(args[args.index('--vram')+1], 0)
lines = []
for offset in range(start, end, 4):
    word = elf[offset:offset+4].hex()
    mnemonic = 'jr $ra' if offset == start else 'nop'
    lines.append(f'/* {offset:06X} {vram+offset-start:08X} {word} */ {mnemonic}\\n')
(out/'fixture.s').write_text(''.join(lines))
''')
        if mutate:
            script = package / '__main__.py'
            script.write_text(script.read_text().replace("word = elf[offset:offset+4].hex()", "word = 'ffffffff'"))
        for name, path in (('m2c', m2c), ('spimdisasm', spim)):
            self.config['tools'][name] = {'revision': commit(path)}
        return m2c, spim

    def args(self, tools, name='fresh'):
        elf = self.repo / 'owned.elf'
        elf.write_bytes(self.data)
        config = self.repo / 'study.json'
        config.write_text(json.dumps(self.config))
        return Namespace(repo_root=self.repo, out=self.repo / 'local/research' / name, elf=elf,
                         m2c=tools[0], spimdisasm=tools[1], config=config, reviewed_patches=None,
                         case=None, timeout=10)

    def test_fresh_extraction_and_decompile_has_distinct_acceptance_levels(self):
        args = self.args(self.fake_tools())
        self.assertEqual(w.execute(args), 0)
        report = json.loads((args.out / 'manifest.json').read_text())
        self.assertEqual(report['workflow_status'], 'executed_requires_review')
        case = report['cases'][0]
        self.assertEqual(case['instruction_verification']['count'], 2)
        self.assertTrue(case['instruction_verification']['complete_extent_matches_elf'])
        self.assertEqual((args.out / '00100000/bytes.bin').read_bytes(), self.data[0x100:0x108])
        for variant in case['variants'].values():
            self.assertEqual(variant['exit_code'], 0)
            self.assertTrue(variant['status']['executed'])
            self.assertEqual(variant['status']['compilable'], 'unreviewed')
            self.assertEqual(variant['status']['bounded_equivalent'], 'unreviewed')
        self.assertEqual(len(report['commands']), 3)
        self.assertTrue(all(c['elapsed_seconds'] >= 0 for c in report['commands']))
        for relative, expected in report['artifact_hashes'].items():
            self.assertEqual(w.sha(args.out / relative), expected)

    def test_wrong_elf_is_refused_with_failure_manifest_and_overwrite_protected(self):
        args = self.args(self.fake_tools())
        args.elf.write_bytes(self.data[:-1] + b'X')
        self.assertEqual(w.execute(args), 1)
        manifest = args.out / 'manifest.json'
        original = manifest.read_bytes()
        self.assertIn('SHA-256 mismatch', json.loads(original)['failure']['message'])
        with self.assertRaisesRegex(w.WorkflowError, 'overwrite'):
            w.execute(args)
        self.assertEqual(manifest.read_bytes(), original)

    def test_wrong_tool_revision_refused_by_actual_git_check(self):
        tools = self.fake_tools()
        self.config['tools']['m2c']['revision'] = '0' * 40
        args = self.args(tools)
        self.assertEqual(w.execute(args), 1)
        self.assertIn('unexpected tool revision', json.loads((args.out / 'manifest.json').read_text())['failure']['message'])
        self.assertFalse((args.out / '00100000').exists())

    def test_dirty_tools_require_exact_reviewed_patch_and_reject_untracked(self):
        m2c, _ = self.fake_tools()
        expected = self.config['tools']['m2c']
        (m2c / 'm2c.py').write_text('print("changed")\n')
        with self.assertRaisesRegex(w.WorkflowError, 'reviewed patch'):
            w.verify_tool(m2c, expected)
        reviewed = {'revision': expected['revision'], 'patch_sha256': w.digest(w.git(m2c, 'diff', '--binary', 'HEAD')), 'review': 'fixture reviewed'}
        result, _ = w.verify_tool(m2c, expected, reviewed)
        self.assertFalse(result['clean'])
        reviewed['patch_sha256'] = '0' * 64
        with self.assertRaisesRegex(w.WorkflowError, 'identity mismatch'):
            w.verify_tool(m2c, expected, reviewed)
        (m2c / 'new-source.py').write_text('')
        with self.assertRaisesRegex(w.WorkflowError, 'untracked'):
            w.verify_tool(m2c, expected, reviewed)

    def test_incomplete_extent_and_boundary_mutations_rejected(self):
        for key, value, message in [('end_exclusive', '0x100004', 'extent'), ('bytes_sha256', '0'*64, 'SHA-256'), ('elf_offset', '0x104', 'offset')]:
            with self.subTest(key=key):
                case = copy.deepcopy(self.case)
                case[key] = value
                with self.assertRaisesRegex(w.WorkflowError, message):
                    w.validate_extent(self.elf, case)
        case = copy.deepcopy(self.case)
        case.update(size=4, end_exclusive='0x100004', bytes_sha256=w.digest(self.data[0x100:0x104]))
        case['ghidra_provenance']['map_row']['span_bytes'] = '4'
        with self.assertRaisesRegex(w.WorkflowError, 'terminal'):
            w.validate_extent(self.elf, case)

    def test_missing_table_control_and_target_mutation(self):
        case = copy.deepcopy(self.case)
        case['required_tables'] = ['jtbl_fixture']
        with self.assertRaisesRegex(w.WorkflowError, 'missing required table'):
            w.validate_tables(self.elf, case, self.config)
        table = {'address': '0x100008', 'end_exclusive': '0x10000c', 'size': 4, 'elf_offset': '0x108',
                 'bytes_sha256': w.digest(self.data[0x108:]), 'entry_width': 4, 'entry_count': 1,
                 'targets': ['0x100000'], 'count_evidence': {'address': '0x100000', 'instruction_bytes_sha256': w.digest(self.data[0x100:0x104])}}
        self.config['tables']['jtbl_fixture'] = table
        self.assertEqual(len(w.validate_tables(self.elf, case, self.config)), 1)
        table['targets'] = ['0x100004']
        with self.assertRaisesRegex(w.WorkflowError, 'table targets'):
            w.validate_tables(self.elf, case, self.config)

    def test_missing_required_table_fails_actual_pipeline_before_tool_runs(self):
        self.case['required_tables'] = ['absent']
        args = self.args(self.fake_tools())
        self.assertEqual(w.execute(args), 1)
        report = json.loads((args.out / 'manifest.json').read_text())
        self.assertIn('missing required table', report['failure']['message'])
        self.assertEqual(report['commands'], [])

    def test_missing_required_data_fails_actual_pipeline(self):
        self.case['required_data'] = ['D_missing']
        args = self.args(self.fake_tools())
        self.assertEqual(w.execute(args), 1)
        report = json.loads((args.out / 'manifest.json').read_text())
        self.assertIn('missing required data', report['failure']['message'])
        self.assertEqual(report['commands'], [])

    def test_selected_cases_automatically_include_required_helper(self):
        helper = copy.deepcopy(self.case)
        helper['id'] = '00100010'
        self.case['required_helpers'] = ['00100010']
        self.config['cases'].append(helper)
        args = self.args(self.fake_tools())
        args.case = ['00100000']
        self.assertEqual(w.execute(args), 0)
        report = json.loads((args.out / 'manifest.json').read_text())
        self.assertEqual(report['requested_cases'], ['00100000'])
        self.assertEqual(report['selected_cases_including_helpers'], ['00100000', '00100010'])
        self.assertEqual(len(report['cases']), 2)

    def test_config_identifier_path_escape_is_rejected(self):
        self.case['id'] = '../../outside'
        args = self.args(self.fake_tools())
        self.assertEqual(w.execute(args), 1)
        report = json.loads((args.out / 'manifest.json').read_text())
        self.assertIn('case IDs', report['failure']['message'])
        self.assertEqual(report['commands'], [])

    def test_output_must_be_ignored_research_and_cannot_escape_via_symlink(self):
        with self.assertRaisesRegex(w.WorkflowError, 'local/research'):
            w.reserve_output(self.repo, self.repo / 'tracked-output')
        (self.repo / '.gitignore').write_text('')
        with self.assertRaisesRegex(w.WorkflowError, 'git-ignored'):
            w.reserve_output(self.repo, self.repo / 'local/research/out')
        (self.repo / '.gitignore').write_text('/local/\n')
        (self.repo / 'local/research').mkdir(parents=True)
        outside = self.base / 'outside'
        outside.mkdir()
        (self.repo / 'local/research/escape').symlink_to(outside, target_is_directory=True)
        with self.assertRaisesRegex(w.WorkflowError, 'local/research'):
            w.reserve_output(self.repo, self.repo / 'local/research/escape/out')

    def test_mutated_emitted_words_fail_and_preserve_artifacts(self):
        args = self.args(self.fake_tools(mutate=True))
        self.assertEqual(w.execute(args), 1)
        report = json.loads((args.out / 'manifest.json').read_text())
        self.assertIn('do not exactly match', report['failure']['message'])
        self.assertTrue((args.out / '00100000/raw.s').exists())
        self.assertEqual(len(report['commands']), 1)
        self.assertEqual(report['cases'][0]['variants'], {})

    def test_source_policy_catches_silently_ignored_ctc1_without_output_errors(self):
        policy = w.source_instruction_policy(POLICY_SOURCE)
        rows = [{'address': '0x100000', 'bytes': '00000000', 'mnemonic': x, 'operands': ''} for x in ('nop', 'ctc1', 'sync.p', 'mfc0', 'vadd.xyzw')]
        diagnostics = w.instruction_diagnostics(rows, policy)
        self.assertEqual([d['mnemonic'] for d in diagnostics], ['ctc1', 'sync.p', 'mfc0', 'vadd.xyzw'])
        self.assertIn('silently ignores', diagnostics[0]['reason'])
        self.assertEqual(w.output_diagnostics(b'int f(void) { return 0; }', b''), [])
        status = w.variant_status(0, bool(diagnostics))
        self.assertTrue(status['exit_zero'])
        self.assertEqual(status['readable_recovery'], 'rejected_semantics')
        self.assertTrue(w.output_diagnostics(b'M2C_ERROR(unknown instruction)', b''))

    def test_missing_instruction_or_address_shift_is_rejected(self):
        text = '/* 000100 00100000 0800E003 */ jr $ra\n/* 000104 00100004 00000000 */ nop\n'
        self.assertEqual(len(w.verify_instructions(text, 0x100000, self.data[0x100:0x108])), 2)
        for bad in (text.splitlines()[0], text.replace('00100004', '00100008')):
            with self.assertRaisesRegex(w.WorkflowError, 'do not exactly match'):
                w.verify_instructions(bad, 0x100000, self.data[0x100:0x108])

if __name__ == '__main__':
    unittest.main()
