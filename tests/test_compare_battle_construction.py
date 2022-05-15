import importlib.util
import json
import subprocess
import sys
import struct
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / 'tools' / 'compare_battle_construction.py'
spec = importlib.util.spec_from_file_location('compare_battle_construction', SCRIPT)
mod = importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)

def wrap(b): return {'bytes':len(b),'raw_hex':b.hex()}
def source_fixture():
    child=bytearray(0x3d0); struct.pack_into('<HH',child,0x42,0,0)
    live=bytes(0x50)
    no_model={'instance':0xffffffff,'present':False}
    entry={'event':'constructor_entry','sequence':0,'constructor_id':1,'kind':'party','body_pointer':1000,'source_slot':0,'formation_ordinal':0,'live_record':wrap(live),'child':wrap(child)}
    done={'event':'constructor_complete','sequence':1,'constructor_id':1,'kind':'party','body_pointer':1000,'source_slot':0,'formation_ordinal':0,'live_record':wrap(live),'child':wrap(child),'model':no_model}
    reg0={'event':'registration_entry','sequence':2,'registration_index':0,'registration_kind':0,'constructor_id':1}
    table=bytearray(0x8010); struct.pack_into('<I',table,0,1)
    reg={'event':'registration_complete','sequence':3,'registration_index':0,'registration_kind':0,'constructor_id':1,'body_pointer':1000,'registered_count':1,'registration_table':wrap(table)}
    head={'schema':mod.SCHEMA,'elf_sha256':'2a90b6af1b5e0ab577e899113d7d35d97669a5','frame_convention':'reference vblank','elf_sha256':'2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5'}
    finish={'event':'finish','party_completed':1,'enemy_completed':0,'registrations_completed':1}
    return head,[entry,done,reg0,reg],finish

def native_fixture():
    child=bytearray(0x3d0)
    return {'schema':'okage-native-battle-construction-v1','frame':10,'complete':True,'error':'','actors':[{'enemy':False,'entity_id':0,'child_hex':child.hex(),'record_before_hex':bytes(0x50).hex(),'record_hex':bytes(0x50).hex(),'random_draws':[],'model':None,'bag':None,'box':None}], 'entities':[{'kind':0,'actor':0,'record_bytes':0x50}]}

def enemy_fixture():
    child=bytearray(0x3e0); struct.pack_into('<HH',child,0x42,0,0)
    def model(instance):
        inst=bytearray(0x3c)
        seq=bytes(0x90)
        return {'instance':instance,'present':True,'instance_record':wrap(inst),'channels':wrap(seq),'context_id':0xffff}
    model_o=model(1); bag_o=model(2); box_o=model(3)
    entry={'event':'constructor_entry','sequence':0,'constructor_id':1,'kind':'enemy','body_pointer':2000,'child':wrap(child)}
    done={'event':'constructor_complete','sequence':1,'constructor_id':1,'kind':'enemy','body_pointer':2000,'child':wrap(child),'model':model_o,'hidden_bag':bag_o,'hidden_box':box_o}
    reg0={'event':'registration_entry','sequence':2,'registration_index':0,'registration_kind':1,'constructor_id':1}
    table=bytearray(0x8010); struct.pack_into('<I',table,0,1)
    reg={'event':'registration_complete','sequence':3,'registration_index':0,'registration_kind':1,'constructor_id':1,'body_pointer':2000,'registered_count':1,'registration_table':wrap(table)}
    head={'schema':mod.SCHEMA,'elf_sha256':'2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5','frame_convention':'reference vblank'}
    table2=bytearray(0x8010); struct.pack_into('<I',table2,0,2)
    null_entry={'event':'registration_entry','sequence':4,'registration_index':1,'registration_kind':4,'constructor_id':None,'body_pointer':0,'stats_pointer':0}
    null_done={'event':'registration_complete','sequence':5,'registration_index':1,'registration_kind':4,'constructor_id':None,'body_pointer':0,'stats_pointer':0,'registered_count':2,'registration_table':wrap(table2)}
    finish={'event':'finish','party_completed':0,'enemy_completed':1,'registrations_completed':2}
    def nm(): return {'has_motion':False,'pc':0,'script_state':0,'flags':0,'visible':False,'channels':[0,0,0,0]}
    actor={'enemy':True,'entity_id':0,'child_hex':child.hex(),'record_before_hex':'','record_hex':'','random_draws':[],'model':nm(),'bag':nm(),'box':nm()}
    native={'schema':'okage-native-battle-construction-v1','frame':1,'complete':True,'error':'','actors':[actor],'entities':[{'kind':1,'actor':0,'record_bytes':0x6c},{'kind':4,'actor':None,'record_bytes':0}]}
    return (head,[entry,done,reg0,reg,null_entry,null_done],finish),native

class ComparatorTests(unittest.TestCase):
    def reference(self): return mod.load_ref_from_rows if hasattr(mod,'load_ref_from_rows') else None
    def loaded(self, rows):
        # Exercise the same strict row parser through an isolated JSONL file.
        h, events, fin=rows
        with tempfile.TemporaryDirectory() as td:
            p=Path(td)/'ref.jsonl'; p.write_text('\n'.join(json.dumps(x) for x in [h,*events,fin])+'\n')
            return mod.load_ref(p)
    def test_exact_synthetic_match(self):
        result=mod.compare(self.loaded(source_fixture()),native_fixture())
        self.assertEqual(result['counts']['differences'],0)
        self.assertEqual(result['counts']['original_actors'],1)
        self.assertEqual(result['counts']['original_registrations'],1)
        self.assertTrue(result['complete'])
    def test_numeric_corruption_is_first_difference(self):
        h,e,f=source_fixture(); child=bytearray.fromhex(e[1]['child']['raw_hex']); struct.pack_into('<I',child,0x20,123); e[1]['child']=wrap(child)
        result=mod.compare(self.loaded((h,e,f)),native_fixture())
        self.assertEqual(result['first_difference']['path'],"actor[('party', 0, 0)].child[020:028]")
    def test_missing_actor_and_registration_reported(self):
        h,e,f=source_fixture(); n=native_fixture(); n['actors']=[]; n['entities']=[]
        result=mod.compare(self.loaded((h,e,f)),n)
        self.assertEqual(result['counts']['missing_actors'],1)
        self.assertEqual(result['counts']['differences'],2)
    def test_duplicate_sequence_rejected(self):
        h,e,f=source_fixture(); e[1]['sequence']=0
        with tempfile.TemporaryDirectory() as td:
            p=Path(td)/'x'; p.write_text('\n'.join(json.dumps(x) for x in [h,*e,f]))
            with self.assertRaises(mod.Invalid): mod.load_ref(p)
    def test_wrong_child_extent_rejected(self):
        h,e,f=source_fixture(); e[1]['child']=wrap(bytes(0x20))
        with self.assertRaises(mod.Invalid): mod.compare(self.loaded((h,e,f)),native_fixture())
    def test_hidden_asset_channels_and_static_context_are_checked(self):
        ref,n=enemy_fixture(); result=mod.compare(self.loaded(ref),n)
        self.assertEqual(result['counts']['differences'],0)
        self.assertEqual(result['compared']['channel_words'],12)
        self.assertEqual(result['compared']['registrations'],2)
        n['actors'][0]['box']['channels'][2]=1
        result=mod.compare(self.loaded(ref),n)
        self.assertTrue(any('.hidden_box.channels' in d['path'] for d in result['differences']))

    def test_null_registration_requires_null_actor_and_zero_record(self):
        ref,n=enemy_fixture(); n['entities'][1]['actor']=3; n['entities'][1]['record_bytes']=0x50
        result=mod.compare(self.loaded(ref),n)
        self.assertTrue(any(d['path']=='entity[1].actor' and d['expected'] is None for d in result['differences']))
        self.assertTrue(any(d['path']=='entity[1].record_bytes' and d['expected']==0 for d in result['differences']))

    def test_registration_actor_comes_from_original_child_id(self):
        ref,n=enemy_fixture(); head,events,finish=ref
        child=bytearray.fromhex(events[1]['child']['raw_hex']); struct.pack_into('<I',child,0x130,0xff000005); events[1]['child']=wrap(child)
        result=mod.compare(self.loaded((head,events,finish)),n)
        row=next(d for d in result['differences'] if d['path']=='entity[0].actor')
        self.assertEqual((row['expected'],row['actual']),(5,0))

    def test_registration_count_corruption_fails_closed(self):
        ref,n=enemy_fixture(); ref[1][3]['registered_count']=2
        with self.assertRaises(mod.Invalid): mod.load_ref_from_rows(ref) if hasattr(mod,'load_ref_from_rows') else self.loaded(ref)

    def test_nonzero_source_collision_instance_is_a_difference(self):
        ref,n=enemy_fixture(); inst=bytearray.fromhex(ref[1][1]['model']['instance_record']['raw_hex']); struct.pack_into('<H',inst,0x32,4); ref[1][1]['model']['instance_record']=wrap(inst)
        result=mod.compare(self.loaded(ref),n)
        self.assertTrue(any('collision_instance_source_zero' in d['path'] and d['actual']==4 for d in result['differences']))

    def test_native_malformed_hex_becomes_input_error(self):
        ref,n=enemy_fixture(); n['actors'][0]['child_hex']='not hex'
        with self.assertRaises(mod.Invalid): mod.compare(self.loaded(ref),n)

    def test_cli_difference_and_bad_input_statuses(self):
        with tempfile.TemporaryDirectory() as td:
            td=Path(td); h,e,f=source_fixture(); ref=td/'ref.jsonl'; native=td/'native.json'; out=td/'out.json'
            ref.write_text('\n'.join(json.dumps(x) for x in [h,*e,f])+'\n')
            n=native_fixture(); n['actors'][0]['record_hex']='ff'*0x50; native.write_text(json.dumps(n))
            cmd=[sys.executable,str(SCRIPT),'--reference',str(ref),'--native',str(native),'--output',str(out)]
            self.assertEqual(subprocess.run(cmd,capture_output=True).returncode,1)
            self.assertGreater(json.loads(out.read_text())['counts']['differences'],0)
            bad=td/'bad.jsonl'; bad.write_text('{}\n')
            self.assertEqual(subprocess.run([sys.executable,str(SCRIPT),'--reference',str(bad),'--native',str(native),'--output',str(td/'badout')],capture_output=True).returncode,2)
            native.write_text('[]')
            self.assertEqual(subprocess.run([sys.executable,str(SCRIPT),'--reference',str(ref),'--native',str(native),'--output',str(td/'badnative')],capture_output=True).returncode,2)

    def test_rng_observation_is_retained_as_difference(self):
        h,e,f=source_fixture(); e.append({'event':'rng_entry','sequence':4,'ra':0x15455c,'constructor_id':1,'rng_state':1})
        # finish row count is derived from constructor/reg counts; sequence remains contiguous.
        n=native_fixture(); result=mod.compare(self.loaded((h,e,f)),n)
        self.assertEqual(result['rng']['constructors'][0]['original_return_addresses'],[0x15455c])
        self.assertTrue(any('sequence lengths differ' in x for x in result['unsupported_checks']))

if __name__=='__main__': unittest.main()
