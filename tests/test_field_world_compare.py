"""Synthetic world source/report controls with 64-byte RAM/VRAM fixtures."""
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import field_world_compare as cmp

def dump(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value) + '\n')

def jsonl(path, values):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(''.join(json.dumps(value) + '\n' for value in values))

class Fixture:
    def __init__(self, root):
        self.original, self.native, self.inventory = root / 'original', root / 'native', root / 'pins.json'
        self.events, self.cp_events, self.calls = [], [], {}
        self.tables, self.scene = [], []
        self.allocated, self.bound, self.completed = False, False, False
        self.raw = {}
        self.enter('main', fn='0x1002f8')
        worker = self.enter('field_worker')
        prefix = self.enter('field_prefix', fn='0x131d28')
        archive = self.enter('archive_mount', fn='0x1bccf0', str='room.xpf')
        self.leave(archive)
        self.checkpoint_call('cache_enable_set', 'field-cache-enabled')
        path = self.enter('xmd_load_path', str='wal.xmd')
        buffer = self.enter('xmd_load')
        alloc = self.enter('alloc', fn='0x1cf008', **{'class': 1}, psm=0, w=8, h=8)
        self.leave(alloc, result={'id': 0, 'tbp': 10})
        self.allocated = True
        self.leave(buffer, result=5)
        self.boundary('world-xmd_load-0001', 'xmd_load')
        material = self.enter('material_table', fn='0x182e28')
        table = {'ptr': '0x100', 'next': '0x0', 'word1': 2, 'word2': 1, 'count': 1, 'handles': [5]}
        self.leave(material, table=table)
        self.tables = [table, {'ptr': '0x80', 'next': '0x0', 'word1': 1, 'word2': 1, 'count': 0, 'handles': []}]
        self.bound = True
        self.leave(path, result=2)
        self.boundary('world-xmd_load_path-0001', 'xmd_load_path')
        self.checkpoint_call('field_map_load', 'field-map-loaded')
        xta = self.enter('xta_load')
        image = self.enter('xta_image_load')
        self.leave(image, v0='0x6')
        self.scene = [6]
        self.boundary('world-xta_image_load-0001', 'xta_image_load')
        self.leave(xta)
        self.boundary('world-xta_load-0001', 'xta_load')
        self.leave(prefix)
        self.cp('field-worker-before-complete', pc='0x1326c0')
        self.completed = True
        self.leave(worker)
        self.boundary('field-worker-return', 'field_worker')
        self.checkpoint_call('field_map_commit', 'field-map-committed')
        self.checkpoint_call('field_activate', 'field-activation-return')
        self.emit(ev='stop', kind='field_world_complete', pc='0x1344b4')
        self.cps = [e['name'] for e in self.cp_events]
        meta = {'events': len(self.events), 'checkpoints': self.cps, 'field_world': True, 'world_stopped': True,
                'missing_checkpoints': [], 'rand_pending_at_stop': False, 'rand_calls': 1, 'open_frames_at_stop': ['main']}
        for run in ('world-fill00', 'world-fillff'):
            dump(self.original / 'runs' / run / 'run-meta.json', meta)
            jsonl(self.original / 'runs' / run / 'events.jsonl', self.events)
        dump(self.native / 'world-route.json', {'route': 'newgame-field-world', 'elf_sha256': cmp.ELF,
                                               'checkpoints': {cp: n for n, cp in enumerate(self.cps)}})
        self.pair = {'pair': ['world-fill00', 'world-fillff'], 'failures': [], 'checkpoints': {}}
        lifecycle = cmp.resource_stream(self.events)
        for e in self.cp_events:
            cp, boundary = e['name'], e['i']
            raw = self.raw[cp]
            d = self.native / cp
            dump(d / 'state.json', cmp.convert_state(raw, cp))
            jsonl(d / 'lifecycle.jsonl', [row for at, row in lifecycle if at < boundary])
            jsonl(d / 'rng.jsonl', [{'i': 0, 'phase': 'session', 'state': 1103527590, 'value': 1103527590}])
            dump(d / 'effects.json', {'effect106': None})
            dump(d / 'session.json', cmp.source_h.session_state(raw))
            dump(d / 'field.json', cmp.source_i.field_json(raw['field'], []))
            world = {'models': [{'name': 'wal.xmd', 'handles': [5]}] if raw['material_tables'] else [],
                     'xta_scene': [6] if self.cps.index(cp) >= self.cps.index('world-xta_image_load-0001') else []}
            dump(d / 'world.json', world)
            masks = {'vram-written-mask.bin': bytes([3]) + bytes(63), 'vram-written-values.bin': bytes([0x5a]) + bytes(63),
                     'vram-invalidated-mask.bin': bytes(64)}
            proof = {'resource_state_equal': cmp.source_f.resource_equal(raw, raw), 'rng_state_equal': True,
                     'raw_ee_ram_equal': True, 'writer_ledger_equal': True, 'ever_upload_ledger_equal': True,
                     'vram': {key: hashlib.sha256(data).hexdigest() for key, data in zip(('mask_sha256','values_sha256','invalidated_sha256'), masks.values())}}
            self.pair['checkpoints'][cp] = proof
            dump(self.original / 'vram-analysis-v1' / cp / 'analysis.json', proof)
            for filename, data in masks.items():
                p = self.original / 'vram-analysis-v1' / cp / filename
                p.write_bytes(data)
                target = dict(zip(masks, ('vram.known', 'vram.bytes', 'vram.invalidated')))[filename]
                (d / target).write_bytes(data)
        dump(self.original / 'vram-analysis-v1/analysis.json', self.pair)
        self.repin()

    def emit(self, **value):
        value = {'i': len(self.events), 'vb': 0, **value}
        self.events.append(value)
        return value

    def enter(self, name, fn=None, **value):
        return self.emit(ev='enter', name=name, fn=fn or cmp.FUNCTIONS[name], **value)

    def leave(self, enter, **value):
        return self.emit(ev='exit', name=enter['name'], fn=enter['fn'], enter=enter['i'], **value)

    def checkpoint_call(self, name, cp):
        enter = self.enter(name)
        self.leave(enter)
        self.boundary(cp, name)

    def boundary(self, cp, name):
        self.emit(ev='field_world_boundary', checkpoint=cp, fn=cmp.FUNCTIONS[name], actual_pc='0x1344b4', return_pc='0x1344b4')
        self.cp(cp)

    def cp(self, name, pc='0x1344b4'):
        if not self.cp_events:
            self.emit(ev='rand', n=0, state=1, frames=[], ra='0x1014f4')
            self.emit(ev='rand_exit', n=0, state=1103527590, v0=1103527590)
            a, b = self.events.pop(), self.events.pop()
            at = len(self.events) - 3
            self.events[at:at] = [b, a]
            for n, e in enumerate(self.events):
                old = e['i']; e['i'] = n
                if e.get('ev') == 'exit' and e.get('name') == 'cache_enable_set':e['enter'] += 2
        event = self.emit(ev='checkpoint', name=name, pc=pc)
        self.cp_events.append(event)
        raw = {k: [] for k in cmp.source_f.RESOURCE_STATE_KEYS}
        raw.update(handles=[], records=[], hash=[], allocations=[], allocation_serials=[], reserved=[9], reserved1=[10, 11], reserve_class=0 if self.completed else 1,
                   free_records=512, free_extras=128, occupancy_hex='00' * 8192, material_tables=copy.deepcopy(self.tables),
                   rand_state_214ec8_58=1103527590, vram={'available':4096,'cursors':[0]*64,'occupancy_sha256':'a'*64,'nodes_sha256':'b'*64},
                   title_effect_slot=-1, title_effect=None,
                   game={'image_counter_337500':1082,'command_24f740':[0,0,0],'name_844':'0'*32,'temp_10_sha256':'c'*64,
                         'fade': dict.fromkeys(('current','target','rate','color'),'0x0')},
                   mode_parent_fields={'phase_20':0,'request_3c':0,'w48':0,'w50':0,'current_40':2,'w44':72,'w4c':0})
        if self.allocated:
            raw['allocation_serials'] = [[0,1]]
            raw['allocations'] = [{'id':0,'live':True,'class':1,'refs':0,'desc':{'psm':0,'w':8,'h':8,'tbp':10,'tbw':1,'raw':'0x1'}}]
        field = {k: 0 for k in cmp.i.CONTROL_FIELD_KEYS | {'w38'}}
        field.update(resource_3c='0x100', w30=int(self.completed), w34=1, w60=36, resource_48=37,
                     w337e70='0x1', cache_1fe310='0x1', lower_1fe30c='0x1', reserve_3abb14=hex(raw['reserve_class']),
                     clut_class_2138c4='0x0', tex0_332430=['0x1','0x2'], env_332450=['0x0']*4, ruw_slots_live=[0]*16)
        raw['field'] = field
        self.raw[name] = raw
        for run in ('world-fill00','world-fillff'):
            d = self.original / 'runs' / run / 'checkpoints' / name
            jsonl(d / 'state.jsonl', [raw])
            ram = bytearray(64)
            struct.pack_into('<I',ram,8,len(self.scene))
            if self.scene:struct.pack_into('<i',ram,16,self.scene[0])
            (d/'ee-ram-32mb.bin').write_bytes(ram)

    def repin(self):
        if self.inventory.exists():self.inventory.unlink()
        cmp.pin(self.original,self.inventory)

class CompareControls(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        for key,value in (('RAM',64),('SCENE_COUNT',8),('SCENE_BASE',16),('QUEUE_WRITE',24),('QUEUE_READ',28),('QUEUE_BASE',32)):
            p=patch.object(cmp,key,value);p.start();self.addCleanup(p.stop)
        self.f=Fixture(Path(self.tmp.name));self.cp=self.f.cps[-1]

    def run_cmp(self):return cmp.run(self.f.original,self.f.native,self.f.inventory,vram_size=64)

    def mutate(self,root,name,fn):
        p=root/self.cp/name;x=json.loads(p.read_text());fn(x);dump(p,x)

    def test_equal(self):
        code,r=self.run_cmp();self.assertEqual((code,r.get('error')),(0,None));self.assertFalse(r['whole_raw_table_parity'])

    def test_class1_selector_not_normalized(self):
        self.mutate(self.f.native,'state.json',lambda x:x.update(reserve_class=1));self.assertEqual(self.run_cmp()[0],1)

    def test_both_reserved_queues_not_interchanged(self):
        def change(x):x['reserved'],x['reserved1']=x['reserved1'],x['reserved']
        self.mutate(self.f.native,'state.json',change);self.assertEqual(self.run_cmp()[0],1)

    def test_fifo_order_matters(self):
        self.mutate(self.f.native,'state.json',lambda x:x['reserved1'].reverse());self.assertEqual(self.run_cmp()[0],1)

    def test_missing_native_file(self):
        (self.f.native/self.cp/'world.json').unlink();self.assertEqual(self.run_cmp()[0],5)

    def test_model_binding_corruption(self):
        self.mutate(self.f.native,'world.json',lambda x:x['models'][0]['handles'].append(99));self.assertEqual(self.run_cmp()[0],1)

    def test_xta_scene_corruption(self):
        self.mutate(self.f.native,'world.json',lambda x:x['xta_scene'].append(99));self.assertEqual(self.run_cmp()[0],1)

    def test_zero_count_tables_disclosed(self):
        code,r=self.run_cmp();self.assertEqual(code,0);self.assertEqual(len(r['checkpoints'][self.cp]['uncovered_zero_count_tables']),1)

    def test_buffer_model_binding_unclaimed(self):
        p=self.f.native/'world-xmd_load-0001'/'world.json';x=json.loads(p.read_text());x['models']=[{'name':'pending.xmd','handles':[]}];dump(p,x)
        code,r=self.run_cmp();self.assertEqual(code,0);self.assertFalse(r['checkpoints']['world-xmd_load-0001']['models_compared'])

    def test_source_raw_common_missing_property(self):
        for run in ('world-fill00','world-fillff'):
            p=self.f.original/'runs'/run/'checkpoints'/self.cp/'state.jsonl';x=cmp.rows(p);x[0].pop('reserved1');jsonl(p,x)
        self.f.repin();self.assertEqual(self.run_cmp()[0],2)

    def test_pair_common_missing_witness_property(self):
        p=self.f.original/'vram-analysis-v1/analysis.json';x=json.loads(p.read_text());x['checkpoints'][self.cp]['resource_state_equal'].pop('reserved1');dump(p,x)
        self.f.repin();self.assertEqual(self.run_cmp()[0],2)

    def test_new_allocation_event_omission(self):
        p=self.f.native/self.cp/'lifecycle.jsonl';x=cmp.rows(p);x=[r for r in x if r['kind']!='alloc'];jsonl(p,x)
        self.assertEqual(self.run_cmp()[0],1)

    def test_unknown_native_event(self):
        p=self.f.native/self.cp/'lifecycle.jsonl';x=cmp.rows(p)+[{'kind':'invented'}];jsonl(p,x);self.assertEqual(self.run_cmp()[0],2)

    def test_known_masks_exact(self):
        p=self.f.native/self.cp/'vram.known';p.write_bytes(bytes(64));self.assertEqual(self.run_cmp()[0],1)

    def test_unknown_values_unclaimed(self):
        p=self.f.native/self.cp/'vram.bytes';x=bytearray(p.read_bytes());x[5]=255;p.write_bytes(x);self.assertEqual(self.run_cmp()[0],0)

    def test_known_value_corruption(self):
        p=self.f.native/self.cp/'vram.bytes';x=bytearray(p.read_bytes());x[0]^=16;p.write_bytes(x);self.assertEqual(self.run_cmp()[0],1)

    def test_only_documented_diagnostic_state_keys_ignored(self):
        self.mutate(self.f.native, 'state.json', lambda x: x.update(later_history='unmodeled later history', rng_draws=123))
        self.assertEqual(self.run_cmp()[0], 0)

    def test_unknown_state_property_rejected(self):
        self.mutate(self.f.native, 'state.json', lambda x: x.update(invented=0))
        self.assertEqual(self.run_cmp()[0], 2)

    def test_observed_queue_liveness_overrides_historical_enqueue(self):
        raw = self.f.raw[self.cp]
        path = Path(self.tmp.name) / 'queue.bin'
        ram = bytearray(64)
        struct.pack_into('<II', ram, 24, 1, 0)
        struct.pack_into('<IIII', ram, 32, 0x132688, 0, 72, 0)
        path.write_bytes(ram)
        self.assertTrue(cmp.queued_field_job(path, raw))
        struct.pack_into('<I', ram, 28, 1)
        path.write_bytes(ram)
        self.assertFalse(cmp.queued_field_job(path, raw))

    def test_reused_effect_slot_preserves_reclaimed_title_lifetime(self):
        raw = {'title_effect_slot': 0, 'title_effect': {'slot': 0, 'type': 91, 'flags': '0x0', 'element_handles': [6], 'job': 4}}
        events = [{'i': 0, 'ev': 'enter', 'name': 'effect_alloc', 'shape': 106},
                  {'i': 1, 'ev': 'exit', 'name': 'effect_alloc', 'enter': 0, 'slot': 0},
                  {'i': 2, 'ev': 'enter', 'name': 'effect_free', 'job_slot': 0},
                  {'i': 3, 'ev': 'enter', 'name': 'effect_alloc', 'shape': 91},
                  {'i': 4, 'ev': 'exit', 'name': 'effect_alloc', 'enter': 3, 'slot': 0}]
        title, uncovered = cmp.inherited_effect106(raw, events, 5)
        self.assertEqual(title['state'], 'reclaimed')
        self.assertEqual(uncovered['shape'], 91)
        with self.assertRaises(cmp.Malformed):
            cmp.inherited_effect106(raw, [e for e in events if e['i'] != 2], 5)

    def test_rng_corruption(self):
        p=self.f.native/self.cp/'rng.jsonl';x=cmp.rows(p);x[0]['value']=1;jsonl(p,x);self.assertEqual(self.run_cmp()[0],1)

    def test_asserted_resources_unproven(self):
        p=self.f.native/self.cp/'lifecycle.jsonl';x=cmp.rows(p);x[0]['asserted']=True;jsonl(p,x);self.assertEqual(self.run_cmp()[0],4)

    def test_source_pin_mismatch(self):
        p=self.f.original/'runs/world-fill00/events.jsonl';p.write_text(p.read_text()+'\n');self.assertEqual(self.run_cmp()[0],2)

    def test_route_omission(self):
        p=self.f.native/'world-route.json';x=json.loads(p.read_text());x['checkpoints'].pop(self.cp);dump(p,x);self.assertEqual(self.run_cmp()[0],2)

    def test_common_new_allocation_event_omission_with_valid_inventory(self):
        for run in ('world-fill00', 'world-fillff'):
            p = self.f.original / 'runs' / run / 'events.jsonl'
            values = cmp.source_events(p)
            for event in values:
                if event.get('name') == 'alloc':event['ev'] = 'ignored'
            jsonl(p, values)
        for cp in self.f.cps:
            p = self.f.native / cp / 'lifecycle.jsonl'
            jsonl(p, [r for r in cmp.rows(p) if r['kind'] != 'alloc'])
        self.f.repin()
        code, result = self.run_cmp()
        self.assertEqual(code, 2)
        self.assertIn('source allocation event coverage', result['error'])

    def test_source_new_return_omission(self):
        p = self.f.original / 'runs/world-fill00/events.jsonl'
        values = cmp.source_events(p)
        next(e for e in values if e.get('name') == 'xta_image_load' and e['ev'] == 'exit')['ev'] = 'ignored'
        jsonl(p, values); self.f.repin()
        self.assertEqual(self.run_cmp()[0], 2)

    def test_undocumented_duplicate_source_property(self):
        p = self.f.original / 'runs/world-fill00/events.jsonl'
        p.write_text('{"i":0,"i":0}\n')
        self.f.repin(); self.assertEqual(self.run_cmp()[0], 2)

    def test_documented_source_parent_disambiguation(self):
        p = Path(self.tmp.name) / 'duplicate.jsonl'
        p.write_text('{"ev":"enter","name":"mode_parent_tick","fn":"0x124818","parent":"main","parent":"0x100"}\n')
        result = cmp.source_events(p)[0]
        self.assertEqual(result['parent'], '0x100')
        self.assertEqual(result['_observed_caller_parent'], 'main')

    def test_source_pair_state_witness_cannot_shrink(self):
        for path in (self.f.original / 'vram-analysis-v1/analysis.json', self.f.original / 'vram-analysis-v1' / self.cp / 'analysis.json'):
            value = json.loads(path.read_text())
            proof = value['checkpoints'][self.cp] if 'checkpoints' in value else value
            proof['resource_state_equal'].pop('vram.cursors')
            dump(path, value)
        self.f.repin(); self.assertEqual(self.run_cmp()[0], 2)

    def test_unverified_pair_metadata_is_unproven(self):
        p = self.f.original / 'vram-analysis-v1/analysis.json'
        value = json.loads(p.read_text()); value['failures'] = ['retained source issue']; dump(p, value)
        self.f.repin(); self.assertEqual(self.run_cmp()[0], 4)

    def test_no_module_global_mutations(self):
        before=cmp.base.VRAM,cmp.base.read_text,cmp.i.CHECKPOINTS,cmp.source_f.RESOURCE_STATE_KEYS
        self.run_cmp();self.assertEqual(before,(cmp.base.VRAM,cmp.base.read_text,cmp.i.CHECKPOINTS,cmp.source_f.RESOURCE_STATE_KEYS))

if __name__=='__main__':unittest.main()
