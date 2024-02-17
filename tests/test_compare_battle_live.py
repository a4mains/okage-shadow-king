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
    tail=bytes(0x170);entities=bytes(0x200);task=bytearray(0x3e0 if enemy else 0x3d0);record=bytes(0x6c if enemy else 0x50)
    bindings=[{'value':blob(bytes(n))} for n in [4,1,2,1,2,2]]
    if enemy:bindings[5]={'present':False,'raw_pointer':0}
    callback_pc=0x15b9a0 if enemy else 0x153a20
    struct.pack_into('<I',task,0,callback_pc)
    actor={'entity_id':0,'type':int(enemy),'task':blob(task),'record':blob(record),'stat_bindings':bindings,
           'model':{'captured_channels':3,'world_channels_raw':blob(bytes(3*0x90))}}
    a={'event':'callback','kind':'parent','edge':'entry','sequence':0,'call_id':1,'frame':0,'entry_pc':0x14a248,
       'root':0x400000,'root_prefix':blob(prefix),
       'root_tail_queue':dict(blob(tail),address=0x408060),
       'registered_entities':dict(blob(entities),address=0x400060),'entity_count':1,
       'battle_entry':blob(bytes(0x38)),'shared_variables_574':blob(bytes(4)),'rng_state_word':123,'actors':[actor]}
    b={'event':'callback','kind':'parent','edge':'entry','sequence':0,'call_id':8,'frame':100,'entry_pc':0x14a248,
       'parent_callback':0x14a248,'parent_phase':0,'party_phase':0,'enemy_phase':0,'party_mask':0,'enemy_mask':0,
       'root_prefix_hex':bytes(0x60).hex(),'root_tail_queue_hex':tail.hex(),'registered_entities_hex':entities.hex(),
       'entry_hex':bytes(0x38).hex(),'shared_variables_574':0,'rng_state_word':123,
       'actors':[{'entity_id':0,'enemy':enemy,'task_hex':bytes(len(task)).hex(),'record_hex':record.hex(),
                  'stat_bindings':[bytes(n).hex() if not(enemy and j==5) else None for j,n in enumerate([4,1,2,1,2,2])],
                  'world_channel_bits':[[0]*4 for _ in range(3)],
                  'first_callback_pc':callback_pc,'saved_first_callback_pc':None}]}
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

    @staticmethod
    def observation(event='rng_boundary', sequence=0, boundary='resource_entry'):
        row = {'event':event, 'frame':100, 'observation_sequence':sequence,
               'boundary':boundary, 'rng_state_word':123}
        if event == 'ui_callback':
            row.update(entry_pc=0x11fec8, raw_pad=[64,64,0,0,128,128],
                       normalization_ran=boundary == 'first_return',
                       normalized_masks={'repeat':0,'pressed':64,'released':0} if boundary == 'first_return' else None,
                       analog_ticks=[0,0], selected_entity=0, selection_state=-1, menu_state=0,
                       target_count=1, pending_action=[0,11,0], shared_command=0,
                       shared_values=[0]*8, shared_changed=[0]*8, formation_count=1,
                       formation_groups=[-1]*8, formation_alive_counts=[0]*8,
                       state_cursor=0, current_layer=0, states=[0xffffffff]*16,
                       layers=[{'index':0,'flags':64,'type':2,'command':0xffffffff,'argument':0,
                                'opacity_bits':0x43000000,'changed':[0]*8,'selection':[0]*8}])
        return row

    def test_native_observations_preserve_paired_callback_comparison(self):
        a,b=fixtures()
        baseline=self.compare(a,b)
        b.insert(1,self.observation())
        b.insert(-1,self.observation('ui_callback',1,'first_return'))
        report=self.compare(a,b)
        self.assertTrue(report['equal'])
        self.assertEqual(report['field_mismatch_count'],baseline['field_mismatch_count'])
        self.assertEqual(report['coverage']['paired_rows'],2)
        coverage=report['coverage']['native']
        self.assertEqual(coverage['auxiliary_observations'],{'rng_boundary':1,'ui_callback':1})
        self.assertFalse(coverage['auxiliary_original_comparison_supported'])

    def test_native_observations_do_not_conceal_callback_errors(self):
        for case in ('sequence','nested','backward','unknown','source'):
            with self.subTest(case=case):
                a,b=fixtures();obs=self.observation()
                if case=='sequence':b[2]['sequence']=3
                if case=='backward':obs['frame']=99
                if case=='unknown':obs['event']='unrecognized'
                if case=='source':a.insert(1,dict(obs,frame=0))
                else:b.insert(2 if case=='nested' else -1,obs)
                with self.assertRaises(tool.Invalid):self.compare(a,b)

    def test_native_observation_identity_and_payload_reject_malformed_values(self):
        changes=[('observation_sequence',1),('observation_sequence',True),('rng_state_word',-1),
                 ('boundary','unknown'),('entry_pc',0x120ad0),('raw_pad',[0]*5),
                 ('raw_pad',[0,0,0,0,256,128]),('normalization_ran',1),
                 ('normalized_masks',None),('analog_ticks',[False,0]),('pending_action',[0]),
                 ('formation_groups',[0x80000000]*8),('selection_state',-0x80000001),
                 ('layers',[{'index':0}])]
        for key,value in changes:
            with self.subTest(key=key,value=value):
                a,b=fixtures();obs=self.observation('ui_callback',0,'first_return');obs[key]=value
                b.insert(-1,obs)
                with self.assertRaises(tool.Invalid):self.compare(a,b)

    def test_missing_finish_remains_failure_with_auxiliary_prefix(self):
        a,b=fixtures();b[-1]=self.observation('ui_callback',0,'first_entry')
        report=self.compare(a,b)
        self.assertFalse(report['equal'])
        self.assertFalse(report['coverage']['native']['finish_present'])
        self.assertEqual(report['coverage']['paired_rows'],2)

    def test_victory_parent_boundaries_and_phase_are_compared(self):
        a, b = fixtures()
        for row in a[1:3]:
            row['entry_pc'] = 0x14a780
            prefix = bytearray.fromhex(row['root_prefix']['raw_hex'])
            struct.pack_into('<II', prefix, 0x20, 0, 2)
            struct.pack_into('<I', prefix, 0, 0x14a780)
            row['root_prefix'] = blob(prefix)
        for row in b[1:3]:
            row.update(entry_pc=0x14a780, parent_callback=0x14a780, parent_phase=2)
        report = self.compare(a, b)
        self.assertTrue(report['equal'])
        self.assertEqual(report['coverage']['paired_rows'], 2)
        b[2]['parent_phase'] = 3
        self.assertEqual(self.compare(a, b)['first_mismatch']['field'], 'parent_phase')

    def test_defeat_parent_boundaries_and_phase_are_compared(self):
        a, b = fixtures()
        for row in a[1:3]:
            row['entry_pc'] = 0x14a9c0
            prefix = bytearray.fromhex(row['root_prefix']['raw_hex'])
            struct.pack_into('<II', prefix, 0x20, 0, 2)
            struct.pack_into('<I', prefix, 0, 0x14a9c0)
            row['root_prefix'] = blob(prefix)
        for row in b[1:3]:
            row.update(entry_pc=0x14a9c0, parent_callback=0x14a9c0, parent_phase=2)
        report = self.compare(a, b)
        self.assertTrue(report['equal'])
        self.assertEqual(report['coverage']['paired_rows'], 2)
        b[2]['parent_phase'] = 3
        self.assertEqual(self.compare(a, b)['first_mismatch']['field'], 'parent_phase')

    def test_parent_identity_scope_remains_exact(self):
        for pc in (0x14a784, 0x14a9c4, 0x123456):
            for side in ('original', 'native'):
                with self.subTest(pc=hex(pc), side=side):
                    a, b = fixtures()
                    for row in (a if side == 'original' else b)[1:3]:
                        row['entry_pc'] = pc
                    with self.assertRaisesRegex(tool.Invalid, 'Unknown callback kind/PC'):
                        self.compare(a, b)

    def test_equal_ignores_pointer_lanes_not_scalars(self):
        a,b=fixtures(True)
        task=bytearray.fromhex(a[1]['actors'][0]['task']['raw_hex']);struct.pack_into('<I',task,0x50,0x765432)
        a[1]['actors'][0]['task']=blob(task)
        r=self.compare(a,b);self.assertTrue(r['equal']);self.assertEqual(r['coverage']['paired_rows'],2)
        b[1]['rng_state_word']=124
        r=self.compare(a,b);self.assertFalse(r['equal']);self.assertEqual(r['first_mismatch']['field'],'rng_state_word')
        self.assertEqual(r['first_mismatch']['key'][0],100)

    def test_callback_ids_and_saved_zero_normalization(self):
        a,b=fixtures()
        for row in a[1:3]:
            task=bytearray.fromhex(row['actors'][0]['task']['raw_hex'])
            struct.pack_into('<I',task,0,0x1562b0)
            struct.pack_into('<I',task,0x28,0x1562b0)
            row['actors'][0]['task']=blob(task)
        for row in b[1:3]:
            row['actors'][0]['first_callback_pc']=0x1562b0
            row['actors'][0]['saved_first_callback_pc']=0x1562b0
        report=self.compare(a,b)
        self.assertTrue(report['equal'])
        self.assertTrue(report['coverage']['callback_identity_supported'])
        #Original zero at TASK+28 normalizes to native null.
        a,b=fixtures()
        self.assertTrue(self.compare(a,b)['equal'])

    def test_callback_timing_is_compared_per_boundary(self):
        a,b=fixtures()
        source=bytearray.fromhex(a[2]['actors'][0]['task']['raw_hex'])
        struct.pack_into('<I',source,0,0x1562b0)
        a[2]['actors'][0]['task']=blob(source)
        b[1]['actors'][0]['first_callback_pc']=0x1562b0
        r=self.compare(a,b)
        self.assertFalse(r['equal'])
        self.assertEqual(r['field_mismatch_count'],2)
        self.assertEqual(r['mismatch_examples'][0]['field'],'actor[0].first_callback_pc')

    def test_callback_identity_rejects_malformed_ids_and_native_pointer_lanes(self):
        for value in [-4,True,0x153a21,0x100000000]:
            with self.subTest(value=value):
                a,b=fixtures();b[1]['actors'][0]['first_callback_pc']=value
                with self.assertRaises(tool.Invalid):self.compare(a,b)
        for offset in (0,0x28):
            with self.subTest(offset=offset):
                a,b=fixtures();task=bytearray.fromhex(b[1]['actors'][0]['task_hex'])
                struct.pack_into('<I',task,offset,0x153a20);b[1]['actors'][0]['task_hex']=task.hex()
                with self.assertRaisesRegex(tool.Invalid,'not normalized'):
                    self.compare(a,b)

    def test_legacy_native_trace_reports_callback_identity_unsupported(self):
        a,b=fixtures()
        for row in b[1:3]:
            row['actors'][0].pop('first_callback_pc')
            row['actors'][0].pop('saved_first_callback_pc')
        report=self.compare(a,b)
        self.assertTrue(report['equal'])
        self.assertFalse(report['coverage']['callback_identity_supported'])
        self.assertIn('Legacy native traces',report['scope']['task_additions'])

    def test_additional_task_vector_and_state_ranges(self):
        a,b=fixtures()
        for lo,hi in [(0x30,0x38),(0xf0,0x110),(0x300,0x308)]:
            source=bytearray.fromhex(a[1]['actors'][0]['task']['raw_hex'])
            native=bytearray.fromhex(b[1]['actors'][0]['task_hex'])
            for at in range(lo,hi): source[at]=native[at]=(at+lo)&0xff
            a[1]['actors'][0]['task']=blob(source);b[1]['actors'][0]['task_hex']=native.hex()
        self.assertTrue(self.compare(a,b)['equal'])
        native=bytearray.fromhex(b[1]['actors'][0]['task_hex']);native[0xf0]^=1
        b[1]['actors'][0]['task_hex']=native.hex()
        self.assertEqual(self.compare(a,b)['first_mismatch']['field'],'actor[0].task[f0:110]')

    @staticmethod
    def graph(a, b, links, count=0, scalar_flag=0):
        source = bytearray.fromhex(a[1]['registered_entities']['raw_hex'])
        native = bytearray.fromhex(b[1]['registered_entities_hex'])
        for data in (source, native):
            struct.pack_into('<I', data, 0x68, scalar_flag)
            struct.pack_into('<H', data, 0x16c, count)
        for slot, owner in links.items():
            struct.pack_into('<I', source, 0x6c+4*slot, a[1]['root']+0xc0+owner*0x200)
        a[1]['registered_entities']['raw_hex'] = source.hex()
        b[1]['registered_entities_hex'] = native.hex()
        b[1]['action_graph_links'] = [[links.get(slot) for slot in range(64)]]

    def test_aliases_duplicate_and_stale_last_slot_normalize_exactly(self):
        a,b=fixtures()
        #All64 embedded nodes exist, even if only entity0 is registered. Source
        #158b70 scans/copies beyond logical count; stale tails must be compared.
        self.graph(a,b,{0:0,1:0,63:63},count=2,scalar_flag=1)
        self.assertTrue(self.compare(a,b)['equal'])
        b[1]['action_graph_links'][0][63]=None
        r=self.compare(a,b)
        self.assertEqual(r['field_mismatch_count'],1)
        self.assertEqual(r['first_mismatch']['field'],'entity[0].links[63]')
        self.assertEqual(r['first_mismatch']['original'],63)

    def test_node_flag_is_scalar_not_waza_pointer(self):
        a,b=fixtures();self.graph(a,b,{0:0},count=1,scalar_flag=0xffffffff)
        self.assertTrue(self.compare(a,b)['equal'])
        native=bytearray.fromhex(b[1]['registered_entities_hex'])
        struct.pack_into('<I',native,0x68,0);b[1]['registered_entities_hex']=native.hex()
        self.assertIn('entity[0][0:6c]',self.compare(a,b)['field_counts'])

    def test_missing_nonempty_graph_reports_coverage_not_success(self):
        a,b=fixtures();self.graph(a,b,{0:0},count=1);del b[1]['action_graph_links']
        r=self.compare(a,b)
        self.assertEqual(r['coverage']['paired_rows'],2)
        self.assertIn('action_graph_observed',r['field_counts'])
        self.assertIn('entity[0].links[0]',r['field_counts'])
        self.assertFalse(r['equal'])

    def test_graph_bounds_and_source_owner_provenance(self):
        for case in ['unaligned','outside','root','address','shape','identity','bool','native_pointer']:
            with self.subTest(case=case):
                a,b=fixtures();self.graph(a,b,{0:0},count=1)
                if case in ['unaligned','outside']:
                    data=bytearray.fromhex(a[1]['registered_entities']['raw_hex'])
                    pointer=a[1]['root']+0xc0+(1 if case=='unaligned' else 64*0x200)
                    struct.pack_into('<I',data,0x6c,pointer)
                    a[1]['registered_entities']['raw_hex']=data.hex()
                elif case=='root':a[1]['root']=0x1fff000
                elif case=='address':a[1]['registered_entities']['address']+=16
                elif case=='shape':b[1]['action_graph_links'][0].pop()
                elif case=='identity':b[1]['action_graph_links'][0][0]=64
                elif case=='bool':b[1]['action_graph_links'][0][0]=True
                else:
                    data=bytearray.fromhex(b[1]['registered_entities_hex']);struct.pack_into('<I',data,0x6c,1)
                    b[1]['registered_entities_hex']=data.hex()
                with self.assertRaises(tool.Invalid):self.compare(a,b)

    def test_descriptor_id_boundaries(self):
        for pointer,identity in [(0,None),(0x237578,0),(0x23dd10,195),
                                 (0x23df98,196),(0x243388,354),(0x243410,355),(0x243b80,369)]:
            self.assertEqual(tool.descriptor_id(pointer,'test'),identity)
        for pointer in [0x237577,0x237579,0x23dd98,0x23df97,0x243c08]:
            with self.assertRaisesRegex(tool.Invalid,'unrepresentable'):tool.descriptor_id(pointer,'test')

    def test_action_record_and_descriptor_identities_and_all_numeric_words(self):
        a,b=fixtures();source=bytearray.fromhex(a[1]['actors'][0]['task']['raw_hex'])
        native=bytearray.fromhex(b[1]['actors'][0]['task_hex'])
        for data in (source,native):
            struct.pack_into('<h',data,0x1fc,200)
            struct.pack_into('<I',data,0x1d8,0x12345678) #last numeric waza word
            struct.pack_into('<I',data,0x204,0x80000000) #exact signed-zero bits
        for at,value in [(0x1a8,0x23cda8),(0x1ac,0x100000),(0x1dc,0x110000),(0x200,0x243410)]:
            struct.pack_into('<I',source,at,value)
        a[1]['actors'][0]['task']=blob(source)
        actor=b[1]['actors'][0];actor.update(task_hex=native.hex(),action_record_id=200,action_motion_id=166,action_secondary_id=355)
        self.assertTrue(self.compare(a,b)['equal'])
        actor['action_record_id']=201
        r=self.compare(a,b);self.assertEqual(r['first_mismatch']['field'],'actor[0].action_record_id')
        actor['action_record_id']=200;native[0x1d8]^=1;actor['task_hex']=native.hex()
        self.assertEqual(self.compare(a,b)['first_mismatch']['field'],'actor[0].task[1b0:1dc]')

    def test_no_motion_does_not_infer_waza_row_and_missing_identity_is_difference(self):
        a,b=fixtures()
        for original,row in [(True,a[1]),(False,b[1])]:
            actor=row['actors'][0];data=bytearray.fromhex(actor['task']['raw_hex'] if original else actor['task_hex'])
            struct.pack_into('<h',data,0x1fc,200)
            if original:actor['task']=blob(data)
            else:actor['task_hex']=data.hex()
        self.assertTrue(self.compare(a,b)['equal'])
        source=bytearray.fromhex(a[1]['actors'][0]['task']['raw_hex']);struct.pack_into('<I',source,0x1a8,0x237578)
        a[1]['actors'][0]['task']=blob(source)
        r=self.compare(a,b)
        self.assertIn('actor[0].action_motion_id',r['field_counts'])
        self.assertIn('actor[0].action_record_id',r['field_counts'])

    def test_unpaired_source_rows_still_validate_graph_identity(self):
        a,b=fixtures();self.graph(a,b,{0:0},count=1)
        source=bytearray.fromhex(a[1]['registered_entities']['raw_hex'])
        struct.pack_into('<I',source,0x6c,0x123456)
        a[1]['registered_entities']['raw_hex']=source.hex()
        for row in b[1:3]:row['frame']=101
        with self.assertRaisesRegex(tool.Invalid,'unrepresentable source node'):self.compare(a,b)

    def test_action_pointer_and_identity_rejections(self):
        for case in ['source','native','bool','extent','waza']:
            with self.subTest(case=case):
                a,b=fixtures()
                if case in ['source','waza']:
                    task=bytearray.fromhex(a[1]['actors'][0]['task']['raw_hex'])
                    struct.pack_into('<I',task,0x1a8,0x237578 if case=='waza' else 0x123456)
                    if case=='waza':struct.pack_into('<h',task,0x1fc,-1)
                    a[1]['actors'][0]['task']=blob(task)
                elif case=='native':
                    task=bytearray.fromhex(b[1]['actors'][0]['task_hex']);struct.pack_into('<I',task,0x1ac,4)
                    b[1]['actors'][0]['task_hex']=task.hex()
                else:b[1]['actors'][0]['action_secondary_id']=True if case=='bool' else 370
                with self.assertRaises(tool.Invalid):self.compare(a,b)

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
