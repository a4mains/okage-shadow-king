"""Synthetic fixed-anchor trace comparison; no game assets or original execution."""
import copy
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest

TOOLS=Path(__file__).resolve().parents[1]/'tools'
sys.path.insert(0,str(TOOLS))
import compare_battle_live as tool

def blob(data):
    return {'address':0x400000,'bytes':len(data),'raw_hex':bytes(data).hex()}

def fixtures(enemy=False):
    prefix=bytearray(0x60);struct.pack_into('<I',prefix,0,0x14a248);struct.pack_into('<I',prefix,0x50,1)
    tail=bytes(0x170);entities=bytes(0x200);task=bytes(0x3e0 if enemy else 0x3d0);record=bytes(0x6c if enemy else 0x50)
    bindings=[{'value':blob(bytes(n))} for n in [4,1,2,1,2,2]]
    if enemy:bindings[5]={'present':False,'raw_pointer':0}
    actor={'entity_id':0,'type':int(enemy),'task':blob(task),'record':blob(record),'stat_bindings':bindings,
           'model':{'captured_channels':3,'world_channels_raw':blob(bytes(3*0x90))}}
    a={'event':'callback','kind':'parent','edge':'entry','sequence':0,'call_id':1,'frame':0,'entry_pc':0x14a248,
       'root_prefix':blob(prefix),'root_tail_queue':blob(tail),'registered_entities':blob(entities),'entity_count':1,
       'battle_entry':blob(bytes(0x38)),'shared_variables_574':blob(bytes(4)),'rng_state_word':123,'actors':[actor]}
    b={'event':'callback','kind':'parent','edge':'entry','sequence':0,'call_id':8,'frame':100,'entry_pc':0x14a248,
       'parent_callback':0x14a248,'parent_phase':0,'party_phase':0,'enemy_phase':0,'party_mask':0,'enemy_mask':0,
       'root_prefix_hex':bytes(0x60).hex(),'root_tail_queue_hex':tail.hex(),'registered_entities_hex':entities.hex(),
       'entry_hex':bytes(0x38).hex(),'shared_variables_574':0,'rng_state_word':123,
       'actors':[{'entity_id':0,'enemy':enemy,'task_hex':task.hex(),'record_hex':record.hex(),
                  'stat_bindings':[bytes(n).hex() if not(enemy and j==5) else None for j,n in enumerate([4,1,2,1,2,2])],
                  'world_channel_bits':[[0]*4 for _ in range(3)]}]}
    nativeprefix=bytearray(0x60);struct.pack_into('<I',nativeprefix,0x50,1);b['root_prefix_hex']=nativeprefix.hex()
    ar=copy.deepcopy(a);ar.update(edge='return',sequence=1)
    br=copy.deepcopy(b);br.update(edge='return',sequence=1)
    return ([{'schema':'okage-battle-live-v1','source_elf_sha256':tool.ELF,'requested_vblanks':2},a,ar,
             {'event':'finish','rows':2,'pairs':1,'completed_vblanks':2}],
            [{'schema':'okage-native-battle-live-v1'},b,br,{'event':'finish'}])

class LiveComparisonTests(unittest.TestCase):
    def compare(self,a,b,anchor=100):
        with tempfile.TemporaryDirectory() as d:
            paths=[Path(d)/'source.jsonl',Path(d)/'native.jsonl']
            for p,rows in zip(paths,[a,b]):p.write_text(''.join(json.dumps(r)+'\n' for r in rows))
            return tool.compare(*paths,anchor)

    def test_equal_ignores_pointer_lanes_not_scalars(self):
        a,b=fixtures(True)
        task=bytearray.fromhex(a[1]['actors'][0]['task']['raw_hex']);struct.pack_into('<I',task,0x50,0x765432)
        a[1]['actors'][0]['task']=blob(task)
        r=self.compare(a,b);self.assertTrue(r['equal']);self.assertEqual(r['coverage']['paired_rows'],2)
        b[1]['rng_state_word']=124
        r=self.compare(a,b);self.assertFalse(r['equal']);self.assertEqual(r['first_mismatch']['field'],'rng_state_word')
        self.assertEqual(r['first_mismatch']['key'][0],100)

    def test_no_shift_fits_mismatched_callback_frames(self):
        a,b=fixtures();b[1]['frame']=101;b[2]['frame']=101
        r=self.compare(a,b)
        self.assertEqual(r['coverage']['paired_rows'],0)
        self.assertEqual(len(r['missing_native']),2);self.assertEqual(len(r['missing_original']),2)
        self.assertFalse(r['equal'])

    def test_missing_actor_is_a_semantic_difference(self):
        a,b=fixtures();b[1]['actors']=[]
        r=self.compare(a,b)
        self.assertFalse(r['equal']);self.assertIn('actor_ids',r['field_counts'])

    def test_partial_native_is_reported_not_success(self):
        a,b=fixtures();r=self.compare(a,b[:2])
        self.assertFalse(r['coverage']['native']['finish_present'])
        self.assertEqual(r['coverage']['native']['unpaired_entry'],(8,'parent',0x14a248))
        self.assertEqual(len(r['missing_native']),1);self.assertFalse(r['equal'])

    def test_channel_output_lane_exact_and_record_difference(self):
        a,b=fixtures();b[2]['actors'][0]['world_channel_bits'][1][3]=0x80000000
        r=self.compare(a,b)
        self.assertEqual(r['first_mismatch']['field'],'actor[0].channel[1][3]')
        self.assertEqual(r['field_mismatch_count'],1)
        self.assertEqual(r['first_mismatch']['native'],0x80000000)

    def test_roster_phase_and_mask_mapping(self):
        a,b=fixtures();owner=bytearray(0x38);struct.pack_into('<III',owner,0x1c,0x10004001,0x18000000,1)
        for row in a[1:3]:row.update(kind='party_roster',entry_pc=0x1524e0,owner_header=blob(owner))
        for row in b[1:3]:row.update(kind='party_roster',entry_pc=0x1524e0,party_phase=1,party_mask=0x10000000)
        self.assertTrue(self.compare(a,b)['equal'])
        b[2]['party_phase']=2
        self.assertEqual(self.compare(a,b)['first_mismatch']['field'],'party_phase')

    def test_malformed_extents_sequences_and_missing_finish(self):
        for mutation in ['extent','sequence','source_finish','enemy_null','wrong_return']:
            with self.subTest(mutation=mutation):
                a,b=fixtures(True)
                if mutation=='extent':b[1]['actors'][0]['record_hex']='00'
                elif mutation=='sequence':b[2]['sequence']=0
                elif mutation=='source_finish':a.pop()
                elif mutation=='enemy_null':a[1]['actors'][0]['stat_bindings'][5]['raw_pointer']=123
                else:b[2]['call_id']=55
                with self.assertRaises(tool.Invalid):self.compare(a,b)

    def test_refuses_dangling_report_symlink(self):
        import subprocess
        with tempfile.TemporaryDirectory() as d:
            report=Path(d)/'report.json';target=Path(d)/'absent.json'
            report.symlink_to(target)
            result=subprocess.run([sys.executable,str(TOOLS/'compare_battle_live.py'),
                str(Path(d)/'missing-source'),str(Path(d)/'missing-native'),
                '--source-anchor','100','--report',str(report)],capture_output=True,text=True)
            self.assertEqual(result.returncode,2)
            self.assertIn('Refusing existing report',result.stderr)
            self.assertFalse(target.exists())

    def test_outside_native_interval_counted(self):
        a,b=fixtures()
        extra=copy.deepcopy(b[1:3])
        for r in extra:r['frame']=99
        for r in b[1:3]:r['sequence']+=2;r['call_id']+=1
        b[1:1]=extra
        r=self.compare(a,b)
        self.assertTrue(r['equal']);self.assertEqual(r['coverage']['native_rows_outside_interval'],2)

if __name__=='__main__':unittest.main()
