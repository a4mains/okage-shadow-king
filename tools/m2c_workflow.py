#!/usr/bin/env python3
"""Hash-bound scalar EE research; never executes original or generated game code.

Setup (fresh environment; no Graphviz executable needed)::
    python3 -m venv local/research/m2c-study-venv
    local/research/m2c-study-venv/bin/pip install graphviz==0.20.3 rabbitizer==1.16.2
    local/research/m2c-study-venv/bin/python tools/m2c_workflow.py \
      --elf local/extracted/SCUS_971.29 --m2c /path/to/m2c \
      --spimdisasm /path/to/spimdisasm --out local/research/m2c-study-v1/fresh-01

The two tool checkouts must be clean at config-pinned revisions. Optional reviewed
patch identity JSON is {tool: {revision, patch_sha256, review}}; patch_sha256 is
SHA-256 of `git diff --binary HEAD` and untracked source is always rejected.
No earlier pilot artifacts are read. Safe Ghidra provenance in the config records
extent review; full original ELF ranges, including any Ghidra-unlisted words,
are decoded and checked. No instruction is deleted or replaced. Exit status zero
means execution only, never readable, compilable or equivalent acceptance.
"""
from __future__ import annotations

import argparse
import ast
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import re
import struct
import subprocess
import sys
import time
from datetime import datetime, timezone

class WorkflowError(ValueError):
    """A refused input or a failed extraction invariant."""

def utc_now():
    return datetime.now(timezone.utc).isoformat()

def digest(data):
    return hashlib.sha256(data).hexdigest()

def sha(path):
    return digest(Path(path).read_bytes())

def number(value):
    return int(value, 0) if isinstance(value, str) else int(value)

def require(condition, message):
    if not condition:
        raise WorkflowError(message)

def git(path, *args):
    proc = subprocess.run(['git', '-C', str(path), *args], capture_output=True)
    require(proc.returncode == 0, f'git command failed at {path}: {proc.stderr.decode(errors="replace")}')
    return proc.stdout

def reserve_output(repo_root, out):
    """Resolve symlinks before enforcing ignored repository research storage."""
    root = Path(repo_root).resolve()
    out = Path(out).resolve()
    research = root / 'local' / 'research'
    require(out != research and research in out.parents, 'output must be under repository local/research')
    require(not out.exists(), f'refusing output overwrite: {out}')
    require(git(root, 'rev-parse', '--show-toplevel').decode().strip() == str(root), 'repo-root must be a git root')
    proc = subprocess.run(['git', '-C', str(root), 'check-ignore', '-q', '--no-index', str(out)], capture_output=True)
    require(proc.returncode == 0, 'output must be git-ignored research storage')
    out.mkdir(parents=True, exist_ok=False)
    return out

class ELF:
    def __init__(self, data, identity):
        self.data = data
        require(digest(data) == identity['sha256'], 'owned ELF SHA-256 mismatch')
        require(len(data) == identity['size'], 'owned ELF size mismatch')
        require(len(data) >= 52 and data[:7] == b'\x7fELF\x01\x01\x01', 'expected ELF32 little-endian version1')
        require(struct.unpack_from('<H', data, 18)[0] == identity['machine'] == 8, 'expected MIPS ELF')
        phoff = struct.unpack_from('<I', data, 28)[0]
        phentsize, phnum = struct.unpack_from('<HH', data, 42)
        require(phentsize >= 32 and phoff + phnum * phentsize <= len(data), 'truncated ELF program headers')
        self.segments = []
        for index in range(phnum):
            segment = struct.unpack_from('<8I', data, phoff + index * phentsize)
            if segment[0] == 1:
                require(segment[1] + segment[4] <= len(data), 'truncated ELF load segment')
                self.segments.append(segment)

    def read(self, address, size):
        require(size > 0, 'empty ELF range')
        matches = [s for s in self.segments if s[2] <= address and address + size <= s[2] + s[4]]
        require(len(matches) == 1, 'range is not uniquely file-backed by PT_LOAD')
        offset = matches[0][1] + address - matches[0][2]
        return self.data[offset:offset + size], offset

def validate_extent(elf, item, *, function=True):
    address, end, size = number(item['address']), number(item['end_exclusive']), item['size']
    require(address % 4 == 0 and end % 4 == 0 and end - address == size, 'incomplete or inconsistent extent')
    raw, offset = elf.read(address, size)
    require(offset == number(item['elf_offset']), 'configured ELF offset mismatch')
    require(digest(raw) == item['bytes_sha256'], 'extent bytes SHA-256 mismatch')
    if 'elf_end_offset' in item:
        require(offset + size == number(item['elf_end_offset']), 'configured ELF end offset mismatch')
    if 'boundary_hashes' in item:
        require(digest(raw[:4]) == item['boundary_hashes']['first_word_sha256'] and digest(raw[-4:]) == item['boundary_hashes']['last_word_sha256'],
                'explicit extent boundary hashes mismatch')
    if function:
        require(size >= 8 and item['terminal'] == 'jr_ra_delay_slot', 'missing reviewed terminal policy')
        require(raw[-8:-4] == bytes.fromhex('0800e003'), 'incomplete extent: missing terminal jr ra and delay slot')
        provenance = item['ghidra_provenance']
        row = provenance['map_row']
        require(number('0x' + row['address']) == address and int(row['span_bytes']) == size,
                'function extent differs from reviewed Ghidra boundary')
    return raw, offset

def validate_tables(elf, case, config):
    result = []
    for name in case.get('required_tables', []):
        require(name in config['tables'], f'missing required table: {name}')
        table = config['tables'][name]
        raw, offset = validate_extent(elf, table, function=False)
        require(table['entry_width'] == 4 and table['entry_count'] * 4 == len(raw), 'invalid required table dimensions')
        targets = list(struct.unpack('<' + 'I' * table['entry_count'], raw))
        require(targets == [number(x) for x in table['targets']], 'table targets differ from reviewed metadata')
        require(all(number(case['address']) <= x < number(case['end_exclusive']) and x % 4 == 0 for x in targets),
                'required table target outside function extent')
        evidence = table['count_evidence']
        require(digest(elf.read(number(evidence['address']), 4)[0]) == evidence['instruction_bytes_sha256'],
                'table count evidence bytes mismatch')
        result.append((name, table, raw, offset))
    return result

def validate_data(elf, case, config):
    result = []
    for name in case.get('required_data', []):
        require(name in config.get('data', {}), f'missing required data: {name}')
        item = config['data'][name]
        raw, offset = validate_extent(elf, item, function=False)
        require(bool(item.get('layout')), 'required data lacks guest layout metadata')
        result.append((name, item, raw, offset))
    return result

def verify_tool(path, expected, reviewed=None):
    path = Path(path).resolve()
    revision = git(path, 'rev-parse', 'HEAD').decode().strip()
    status = git(path, 'status', '--porcelain', '--untracked-files=all').decode()
    require(revision == expected['revision'], f'unexpected tool revision at {path}')
    patch = git(path, 'diff', '--binary', 'HEAD')
    require(not any(line.startswith('??') for line in status.splitlines()), 'untracked tool files are not reviewed')
    identity = {'path': str(path), 'revision': revision, 'status': status, 'patch_sha256': digest(patch), 'clean': not status}
    if status:
        require(reviewed is not None, f'dirty tool checkout requires explicit reviewed patch identity: {path}')
        require(reviewed.get('revision') == revision and reviewed.get('patch_sha256') == digest(patch)
                and bool(reviewed.get('review')), 'reviewed tool patch identity mismatch')
        identity['reviewed_patch'] = reviewed
    return identity, patch

def source_instruction_policy(source):
    """Derive instruction coverage and ignored semantics from pinned m2c source.

    Maps contain lambda values, so inspect AST keys without importing/executing
    m2c. Parse's explicit mnemonic comparisons cover control-flow special cases.
    Unknown and ignored non-nop instructions are blockers even if output is clean.
    """
    tree = ast.parse(source)
    known, ignored = set(), set()
    for node in ast.walk(tree):
        if isinstance(node, (ast.Assign, ast.AnnAssign)):
            targets = node.targets if isinstance(node, ast.Assign) else [node.target]
            for target in targets:
                if isinstance(target, ast.Name) and target.id.startswith('instrs_'):
                    values = node.value.keys if isinstance(node.value, ast.Dict) else getattr(node.value, 'elts', [])
                    strings = {x.value for x in values if isinstance(x, ast.Constant) and isinstance(x.value, str)}
                    known.update(strings)
                    if target.id == 'instrs_ignore':
                        ignored.update(strings)
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == 'parse':
            for child in ast.walk(node):
                if isinstance(child, ast.Compare) and isinstance(child.left, ast.Name) and child.left.id in ('mnemonic', 'base_mult_mnemonic'):
                    for operand in child.comparators:
                        values = operand.elts if isinstance(operand, (ast.Tuple, ast.List, ast.Set)) else [operand]
                        known.update(x.value for x in values if isinstance(x, ast.Constant) and isinstance(x.value, str))
    require('ctc1' in ignored and 'nop' in ignored and 'jr' in known, 'unrecognized pinned m2c instruction policy')
    return {'known': sorted(known), 'ignored': sorted(ignored), 'source_sha256': digest(source.encode()),
            'method': 'AST instruction-map keys and explicit parse mnemonic cases; conservative unknown classification'}

ROW = re.compile(r'/\*\s+[0-9A-Fa-f]+\s+([0-9A-Fa-f]{8})\s+([0-9A-Fa-f]{8})\s+\*/\s*(\S+)([^\n]*)')

def verify_instructions(assembly, address, raw):
    rows = list(ROW.finditer(assembly))
    expected = [(f'{address+i:08X}', raw[i:i+4].hex().upper()) for i in range(0, len(raw), 4)]
    actual = [(m[1].upper(), m[2].upper()) for m in rows]
    require(actual == expected, 'emitted instruction addresses/bytes do not exactly match full ELF extent')
    return [{'address': '0x' + m[1].lower(), 'bytes': m[2].lower(), 'mnemonic': m[3], 'operands': m[4].strip()} for m in rows]

def instruction_diagnostics(rows, policy):
    known, ignored = set(policy['known']), set(policy['ignored'])
    diagnostics = []
    for row in rows:
        mnemonic = row['mnemonic']
        normalized = mnemonic[:-1] if mnemonic.endswith('1') and mnemonic[:-1] in ('mult', 'multu', 'div', 'divu', 'mfhi', 'mflo', 'mthi', 'mtlo', 'madd', 'maddu') else mnemonic
        reason = None
        if mnemonic in ignored and mnemonic != 'nop':
            reason = 'm2c silently ignores non-nop instruction semantics'
        elif normalized not in known:
            reason = 'mnemonic absent from pinned m2c instruction coverage'
        elif mnemonic in ('mfc0', 'mtc0', 'sync', 'sync.p', 'cache', 'pref'):
            reason = 'unsupported hardware/control semantics require rejection'
        if reason:
            diagnostics.append({**row, 'severity': 'semantic_blocker', 'reason': reason})
    return diagnostics

def output_diagnostics(stdout, stderr):
    return [line for line in (stdout + b'\n' + stderr).decode(errors='replace').splitlines()
            if re.search(r'M2C_ERROR|M2C_SYNC|unknown instruction|unsupported|Traceback|DecompFailure|error|failure|failed', line, re.I)]

def variant_status(exit_code, blockers):
    return {'executed': exit_code is not None, 'exit_zero': exit_code == 0,
            'readable_recovery': 'decompiler_failed' if exit_code != 0 else 'rejected_semantics' if blockers else 'unreviewed',
            'compilable': 'unreviewed', 'bounded_equivalent': 'unreviewed',
            'acceptance_note': 'Exit zero and absent markers are not semantic acceptance; compile/equivalence evidence is external.'}

class Runner:
    def __init__(self, out, repo, report, timeout):
        self.out, self.repo, self.report, self.timeout = out, repo, report, timeout

    def run(self, argv, prefix, pythonpath):
        argv = [str(x) for x in argv]
        env = dict(os.environ, PYTHONPATH=str(pythonpath), PYTHONNOUSERSITE='1')
        started, clock = utc_now(), time.monotonic()
        stdout, stderr = b'', b''
        code, timed_out = None, False
        try:
            proc = subprocess.run(argv, cwd=self.repo, env=env, capture_output=True, timeout=self.timeout)
            stdout, stderr, code = proc.stdout, proc.stderr, proc.returncode
        except subprocess.TimeoutExpired as exc:
            stdout, stderr, timed_out = exc.stdout or b'', exc.stderr or b'', True
        self.out.joinpath(prefix + '.stdout').write_bytes(stdout)
        self.out.joinpath(prefix + '.stderr').write_bytes(stderr)
        self.report['commands'].append({'argv': argv, 'cwd': str(self.repo), 'environment': {'PYTHONPATH': str(pythonpath), 'PYTHONNOUSERSITE': '1'},
                                        'started_at': started, 'ended_at': utc_now(), 'elapsed_seconds': time.monotonic() - clock,
                                        'exit_code': code, 'timed_out': timed_out,
                                        'stdout': prefix + '.stdout', 'stderr': prefix + '.stderr'})
        return code, stdout, stderr

def symbol_text(config):
    lines = [f'func_{case["id"].upper()} = {case["address"]}; // type:func size:{hex(case["size"])}\n' for case in config['cases']]
    lines.extend(f'{item["name"]} = {item["address"]}; // type:{"func" if item["kind"] == "function" else "s32"}\n' for item in config.get('symbols', []))
    return ''.join(lines)

def run_case(case, config, elf, args, out, runner, policy, patched):
    sid = case['id']
    folder = out / sid
    folder.mkdir()
    raw, offset = validate_extent(elf, case)
    tables = validate_tables(elf, case, config)
    data_inputs = validate_data(elf, case, config)
    folder.joinpath('bytes.bin').write_bytes(raw)
    folder.joinpath('context.c').write_text(config['context'])
    folder.joinpath('symbols.txt').write_text(symbol_text(config))
    item = {'id': sid, 'address': case['address'], 'end_exclusive': case['end_exclusive'], 'size': len(raw), 'elf_offset': hex(offset),
            'bytes_sha256': digest(raw), 'rationale': case['rationale'], 'ghidra_provenance': case['ghidra_provenance'],
            'variants': {}, 'tables': [], 'data': [], 'diagnostics': []}
    # Append immediately so extraction/decompiler failure remains attributable.
    runner.report['cases'].append(item)
    for name, metadata, contents, data_offset in data_inputs:
        folder.joinpath(name + '.bin').write_bytes(contents)
        item['data'].append({'name': name, **metadata, 'verified_elf_offset': hex(data_offset), 'bytes': f'{sid}/{name}.bin'})
    common = [sys.executable, '-m', 'spimdisasm', 'singleFileDisasm', args.elf, folder / 'disasm',
              '--start', hex(offset), '--end', hex(offset + len(raw)), '--vram', case['address'], '--endian', 'little',
              '--abi', 'EABI64', '--named-registers', '--Mreg-names', 'n64', '--instr-category', 'r5900', '--compiler', 'EEGCC',
              '--symbol-addrs', folder / 'symbols.txt', '--no-libultra-syms', '--no-ique-syms', '--no-hardware-regs', '--no-write-binary']
    code, _, _ = runner.run(common, f'{sid}/spimdisasm', args.spimdisasm)
    require(code == 0, f'{sid}: spimdisasm failed; outputs retained')
    generated = list(folder.joinpath('disasm').glob('*.s'))
    require(len(generated) == 1, f'{sid}: expected exactly one disassembly file')
    assembly = generated[0].read_text()
    folder.joinpath('raw.s').write_bytes(generated[0].read_bytes())
    rows = verify_instructions(assembly, number(case['address']), raw)
    folder.joinpath('instructions.json').write_text(json.dumps(rows, indent=2) + '\n')
    item['instruction_verification'] = {'complete_extent_matches_elf': True, 'count': len(rows), 'rows': f'{sid}/instructions.json', 'assembly_sha256': sha(folder / 'raw.s')}
    item['diagnostics'] = instruction_diagnostics(rows, policy)

    def decompile(name, asm, extra=(), context=False):
        cmd = [sys.executable, args.m2c / 'm2c.py', '--target', config['target'], '--no-cache', '--stacktrace']
        if context:
            cmd += ['--valid-syntax', '--context', folder / 'context.c']
        cmd += [asm, *extra]
        exit_code, stdout, stderr = runner.run(cmd, f'{sid}/{name}.m2c', args.m2c)
        cfile = folder / (name + '.generated.c')
        cfile.write_bytes(stdout)
        markers = output_diagnostics(stdout, stderr)
        item['variants'][name] = {'assembly': str(asm.relative_to(out)), 'generated_c': str(cfile.relative_to(out)),
                                  'generated_c_sha256': digest(stdout), 'exit_code': exit_code,
                                  'output_diagnostics': markers, 'tool_mode': 'reviewed-patched' if patched else 'pinned-clean',
                                  'context_assisted': context, 'status': variant_status(exit_code, bool(markers or item['diagnostics']))}

    decompile('raw', folder / 'raw.s')
    decompile('context', folder / 'raw.s', context=True)
    if tables:
        labels = folder / 'target-labels.csv'
        labels.write_text(''.join(f'{number(x):08X},.L{number(x):08X},@jumptablelabel,-\n' for _, table, _, _ in tables for x in table['targets']))
        rodata = folder / 'tables.rodata.s'
        lines = ['.section .rodata\n.balign 4\n']
        for name, table, data, table_offset in tables:
            folder.joinpath(name + '.bin').write_bytes(data)
            lines += [f'glabel {name}\n', *[f'    .word .L{number(x):08X}\n' for x in table['targets']]]
            item['tables'].append({'name': name, **table, 'verified_elf_offset': hex(table_offset), 'bytes': f'{sid}/{name}.bin'})
        rodata.write_text(''.join(lines))
        cmd = list(common)
        cmd[5] = folder / 'disasm-table'
        cmd += ['--variables', labels]
        code, _, _ = runner.run(cmd, f'{sid}/table.spimdisasm', args.spimdisasm)
        require(code == 0, f'{sid}: table disassembly failed')
        paths = list(folder.joinpath('disasm-table').glob('*.s'))
        require(len(paths) == 1, f'{sid}: missing table-assisted assembly')
        asm = folder / 'table-assisted.s'
        asm.write_bytes(paths[0].read_bytes())
        verify_instructions(asm.read_text(), number(case['address']), raw)
        require(all(f'.L{number(x):08X}' in asm.read_text() for _, table, _, _ in tables for x in table['targets']), 'missing table target labels')
        decompile('table-assisted', asm, [rodata])
        decompile('table-context', asm, [rodata], context=True)
    return item

def execute(args):
    repo = args.repo_root.resolve()
    out = reserve_output(repo, args.out)
    start, clock = utc_now(), time.monotonic()
    report = {'schema_version': 1, 'started_at': start, 'workflow_status': 'running', 'commands': [], 'cases': [],
              'python': sys.version, 'python_executable': sys.executable, 'platform': platform.platform(),
              'instruction_policy': 'All words across the full pinned ELF extent are retained. No opcode deletion or replacement.',
              'acceptance': 'Execution only. Readability, compilation and bounded equivalence require independent evidence.'}
    exit_code = 0
    try:
        out.joinpath('workflow.executed.py').write_bytes(Path(__file__).read_bytes())
        report['script_sha256'] = sha(__file__)
        out.joinpath('config.executed.json').write_bytes(args.config.read_bytes())
        report['config'] = {'path': str(args.config), 'sha256': sha(args.config)}
        config = json.loads(args.config.read_text())
        require(config['schema_version'] == 1, 'unsupported config schema')
        identifiers = [case['id'] for case in config['cases']]
        require(len(identifiers) == len(set(identifiers)) and all(re.fullmatch(r'[0-9a-f]{8}', sid) for sid in identifiers),
                'case IDs must be unique eight-digit lowercase hex names')
        names = list(config.get('tables', {})) + list(config.get('data', {})) + [s['name'] for s in config.get('symbols', [])]
        require(all(re.fullmatch(r'[A-Za-z_][A-Za-z_0-9]*', name) for name in names), 'unsafe table/data/symbol name')
        report['repository'] = {'path': str(repo), 'revision': git(repo, 'rev-parse', 'HEAD').decode().strip(),
                                'status': git(repo, 'status', '--short').decode()}
        data = args.elf.read_bytes()
        report['elf'] = {'path': str(args.elf), 'sha256': digest(data), 'size': len(data), 'expected': config['elf']}
        elf = ELF(data, config['elf'])
        reviewed = json.loads(args.reviewed_patches.read_text()) if args.reviewed_patches else {}
        report['tools'] = {}
        for name in ('m2c', 'spimdisasm'):
            identity, patch = verify_tool(getattr(args, name), config['tools'][name], reviewed.get(name))
            report['tools'][name] = identity
            if patch:
                out.joinpath(name + '.reviewed.patch').write_bytes(patch)
        report['dependencies'] = {}
        for package, version in config['dependencies'].items():
            actual = importlib.metadata.version(package)
            require(actual == version, f'dependency pin mismatch: {package}: {actual} != {version}')
            report['dependencies'][package] = actual
        report['dependency_snapshot'] = sorted((d.metadata['Name'] or '', d.version) for d in importlib.metadata.distributions())
        source_path = args.m2c / 'm2c' / 'arch_mips.py'
        out.joinpath('m2c-arch-mips.executed.py').write_bytes(source_path.read_bytes())
        policy = source_instruction_policy(source_path.read_text())
        out.joinpath('instruction-policy.json').write_text(json.dumps(policy, indent=2) + '\n')
        report['instruction_source_policy'] = policy
        requested = set(args.case or [c['id'] for c in config['cases']])
        catalog = {c['id']: c for c in config['cases']}
        require(requested <= set(catalog), 'unknown selected case')
        selected_ids = set(requested)
        while True:
            helpers = {h for sid in selected_ids for h in catalog[sid].get('required_helpers', [])}
            require(helpers <= set(catalog), 'missing required helper metadata')
            if helpers <= selected_ids:
                break
            selected_ids.update(helpers)
        selected = [c for c in config['cases'] if c['id'] in selected_ids]
        report['requested_cases'] = sorted(requested)
        report['selected_cases_including_helpers'] = [c['id'] for c in selected]
        for case in selected:
            validate_extent(elf, case)
            validate_tables(elf, case, config)
            validate_data(elf, case, config)
            require(all(h in {c['id'] for c in config['cases']} for h in case.get('required_helpers', [])), 'missing required helper metadata')
        runner = Runner(out, repo, report, args.timeout)
        for case in selected:
            run_case(case, config, elf, args, out, runner, policy, any(not t['clean'] for t in report['tools'].values()))
        report['workflow_status'] = 'executed_requires_review'
    except (WorkflowError, OSError, ValueError, KeyError, importlib.metadata.PackageNotFoundError) as exc:
        report['workflow_status'] = 'failed_preserved'
        report['failure'] = {'type': type(exc).__name__, 'message': str(exc)}
        exit_code = 1
    finally:
        report['ended_at'] = utc_now()
        report['elapsed_seconds'] = time.monotonic() - clock
        report['artifact_hashes'] = {str(p.relative_to(out)): sha(p) for p in sorted(out.rglob('*')) if p.is_file() and p.name != 'manifest.json'}
        out.joinpath('manifest.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'manifest': str(out / 'manifest.json'), 'workflow_status': report['workflow_status'], 'failure': report.get('failure'), 'elapsed_seconds': report['elapsed_seconds']}))
    return exit_code

def main(argv=None):
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for name in ('elf', 'm2c', 'spimdisasm', 'out'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--config', type=Path, default=root / 'config' / 'm2c-study.json')
    parser.add_argument('--repo-root', type=Path, default=root)
    parser.add_argument('--case', action='append', help='eight-digit config case ID; repeat to select cases')
    parser.add_argument('--reviewed-patches', type=Path)
    parser.add_argument('--timeout', type=float, default=90.0)
    args = parser.parse_args(argv)
    for name in ('elf', 'm2c', 'spimdisasm', 'out', 'config', 'repo_root'):
        setattr(args, name, getattr(args, name).resolve())
    try:
        return execute(args)
    except (WorkflowError, OSError) as exc:
        print(f'm2c workflow refused: {exc}', file=sys.stderr)
        return 1

if __name__ == '__main__':
    raise SystemExit(main())
