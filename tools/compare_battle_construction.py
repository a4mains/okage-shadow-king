#!/usr/bin/env python3
"""Compare natural original battle-constructor observations with a native report."""
import argparse
import json
import os
import struct
import sys
from pathlib import Path

SCHEMA = 'okage-battle-construction-v2'
COMMON = [(0x20,0x28),(0x2c,0x30),(0x40,0x50),(0x60,0x70),(0x90,0xa0),(0xc0,0xd0),
 (0x11c,0x120),(0x128,0x13c),(0x140,0x14c),(0x164,0x17a),(0x17c,0x192),(0x194,0x1a0),
 (0x1a4,0x1a8),(0x21c,0x228),(0x2f0,0x2fc),(0x300,0x308),(0x310,0x344),(0x358,0x3bc)]
EXTRA = {'party':[(0x344,0x348),(0x3bc,0x3c0),(0x1d4,0x1d8),(0x1a8,0x1ac),(0x1fc,0x1fe)],
         'enemy':[(0x3c8,0x3cc),(0x3cd,0x3d1),(0x3d2,0x3d4)]}
# Return addresses immediately after the observed calls to GameRandom.
RNG_CALLERS = {0x15455c, 0x154588, 0x15b794, 0x15c1cc}

class Invalid(ValueError): pass

def need(cond, msg):
    if not cond: raise Invalid(msg)

def raw(obj, name, size=None):
    need(isinstance(obj,dict) and isinstance(obj.get('raw_hex'),str), f'{name}: missing raw_hex')
    try: b=bytes.fromhex(obj['raw_hex'])
    except ValueError as e: raise Invalid(f'{name}: invalid hex: {e}')
    need(len(obj['raw_hex']) == len(b)*2, f'{name}: noncanonical hex')
    if size is not None: need(len(b)==size, f'{name}: expected {size} bytes, got {len(b)}')
    need(obj.get('bytes') == len(b), f'{name}: byte extent mismatch')
    return b

def u16(b, off): return struct.unpack_from('<H',b,off)[0]
def u32(b, off): return struct.unpack_from('<I',b,off)[0]
def load_ref(path):
    lines=Path(path).read_text().splitlines(); need(len(lines)>=2,'reference is empty')
    try: rows=[json.loads(x) for x in lines]
    except Exception as e: raise Invalid(f'invalid reference JSONL: {e}')
    h=rows[0]; need(isinstance(h,dict),'reference header must be an object')
    need(isinstance(rows[-1],dict),'reference finish row must be an object')
    need(h.get('schema')==SCHEMA,'reference schema must be v2')
    need(h.get('elf_sha256')=='2a90b6af1b5e0ab577e899113d7d35d97669a5c13aee2ae848312bd04669aaa5','unexpected reference ELF hash')
    need(rows[-1].get('event')=='finish','reference missing finish row')
    ev=rows[1:-1]; need(all(isinstance(x,dict) for x in ev),'malformed reference event')
    seq=[x.get('sequence') for x in ev]; need(seq==list(range(len(ev))),'reference sequence missing, duplicate, or out of order')
    constructors={}; registrations={}; rng=[]
    for x in ev:
        typ=x.get('event'); ident=x.get('constructor_id')
        if typ in ('constructor_entry','constructor_complete'):
            need(ident not in constructors or typ=='constructor_complete','duplicate constructor entry')
            if typ=='constructor_entry': constructors[ident]={'entry':x}
            else:
                need(ident in constructors and 'complete' not in constructors[ident],'unpaired/duplicate constructor completion')
                constructors[ident]['complete']=x
        elif typ in ('registration_entry','registration_complete'):
            i=x.get('registration_index'); need(isinstance(i,int),'bad registration index')
            if typ=='registration_entry': need(i not in registrations,'duplicate registration entry'); registrations[i]={'entry':x}
            else: need(i in registrations and 'complete' not in registrations[i],'unpaired registration completion'); registrations[i]['complete']=x
        elif typ=='rng_entry': rng.append(x)
        else: raise Invalid(f'unsupported reference event {typ!r}')
    need(all('complete' in v for v in constructors.values()),'constructor missing completion')
    need(all('complete' in v for v in registrations.values()),'registration missing completion')
    need(len(constructors)==rows[-1].get('party_completed',0)+rows[-1].get('enemy_completed',0),'finish constructor counts disagree')
    need(len(registrations)==rows[-1].get('registrations_completed'),'finish registration count disagrees')
    need(sorted(registrations)==list(range(len(registrations))),'registration indices are not contiguous')
    for index,pair in registrations.items():
        c=pair['complete']; need(c.get('registered_count')==index+1,f'registration {index}: registered_count mismatch')
        table=raw(c.get('registration_table'),f'registration {index} table',0x8010)
        need(u32(table,0)==index+1,f'registration {index}: table count mismatch')
        cid=c.get('constructor_id')
        if cid is None:
            need(c.get('body_pointer')==0 and c.get('stats_pointer')==0,
                 f'registration {index}: null row has nonzero body/stats pointer')
        else:
            need(cid in constructors,f'registration {index}: unknown constructor id {cid}')
            constructor=constructors[cid]['complete']
            need(c.get('body_pointer')==constructor.get('body_pointer'),
                 f'registration {index}: body pointer does not match completed constructor')
    registered={p['complete'].get('constructor_id') for p in registrations.values() if p['complete'].get('constructor_id') is not None}
    need(registered==set(constructors),'constructor and registration sets disagree')
    return h,constructors,registrations,rng,rows[-1]

def identity(pair):
    c=pair['complete']; kind=c['kind']; need(kind in ('party','enemy'),'bad constructor kind')
    child=raw(c['child'],'child',0x3d0 if kind=='party' else 0x3e0)
    ordinal=u16(child,0x42); source=u16(child,0x44)
    if kind=='party': need(source==c['source_slot'] and ordinal==c['formation_ordinal'],'party identity disagrees with child bytes')
    return kind,source,ordinal

def compare(reference,native):
    _,ctors,regs,rng,finish=reference
    need(isinstance(native,dict),'native report must be an object')
    need(native.get('schema')=='okage-native-battle-construction-v1','unsupported native schema')
    diffs=[]; unsupported=[]; compared={'bytes':0,'channel_words':0,'registrations':0,'motion_contexts':0}
    def diff(path,a,b,why='value mismatch'):
        if a!=b: diffs.append({'path':path,'expected':a,'actual':b,'reason':why})
    actors=native.get('actors'); need(isinstance(actors,list),'native actors must be array')
    native_by_key={}
    for i,a in enumerate(actors):
        need(isinstance(a,dict),'native actor malformed')
        need(isinstance(a.get('enemy'),bool),f'native actor {i} enemy flag must be boolean')
        try: ch=bytes.fromhex(a.get('child_hex',''))
        except (ValueError,TypeError) as e: raise Invalid(f'native actor {i} child hex malformed: {e}')
        need(len(ch)==(0x3e0 if a.get('enemy') else 0x3d0),f'native actor {i} child extent/kind mismatch')
        key=('enemy' if a.get('enemy') else 'party',u16(ch,0x44),u16(ch,0x42))
        need(key not in native_by_key,f'duplicate native actor {key}')
        native_by_key[key]=a
    expected={}
    ctor_map={}
    for cid,pair in ctors.items():
        key=identity(pair); need(key not in expected,f'duplicate original actor {key}'); expected[key]=pair; ctor_map[cid]=key
    missing=sorted(set(expected)-set(native_by_key)); unexpected=sorted(set(native_by_key)-set(expected))
    for k in missing: diff(f'actors[{k}]', 'present','missing','missing native actor')
    for k in unexpected: diff(f'actors[{k}]','absent','present','unexpected native actor')
    for key,pair in expected.items():
        if key not in native_by_key: continue
        a=native_by_key[key]; kind=key[0]; ent=pair['entry']; comp=pair['complete']
        diff(f'actor[{key}].kind',kind,'enemy' if a.get('enemy') else 'party')
        before=raw(ent['live_record'],'original live record',0x50) if kind=='party' else None
        after=raw(comp['live_record'],'original live record',0x50) if kind=='party' else None
        if kind=='party':
            diff(f'actor[{key}].record_before',before.hex(),a.get('record_before_hex','').lower())
            diff(f'actor[{key}].record',after.hex(),a.get('record_hex','').lower())
            compared['bytes'] += len(before)+len(after)
        child=raw(comp['child'],'original child',0x3d0 if kind=='party' else 0x3e0)
        try: nchild=bytes.fromhex(a['child_hex'])
        except Exception as e: raise Invalid(f'native child hex malformed for {key}: {e}')
        need(len(nchild)==len(child),f'native child extent mismatch for {key}')
        for lo,hi in COMMON+EXTRA[kind]:
            diff(f'actor[{key}].child[{lo:03x}:{hi:03x}]',child[lo:hi].hex(),nchild[lo:hi].hex())
            compared['bytes'] += hi-lo
        for name,nname in [('model','model'),('hidden_bag','bag'),('hidden_box','box')]:
            if name not in comp: continue
            orig=comp[name]; native_model=a.get(nname)
            present=orig.get('present',False)
            diff(f'actor[{key}].{name}.presence',present,native_model is not None)
            if not present or native_model is None: continue
            inst=raw(orig['instance_record'],f'{name} instance',0x3c)
            nm=native_model
            compared['bytes'] += 8
            diff(f'actor[{key}].{name}.visible',bool(u16(inst,0)&0x800),nm.get('visible'))
            diff(f'actor[{key}].{name}.render_flags',u32(inst,0x24)&0x200083,nm.get('flags',0)&0x200083)
            collision=u16(inst,0x32)
            diff(f'actor[{key}].{name}.collision_instance_source_zero',0,collision)
            unsupported.append(f'actor {key} {name}: native report does not serialize collision instance')
            need(isinstance(nm.get('channels'),list),f'native {name} channels missing')
            channels=raw(orig['channels'],f'{name} channels')
            need(len(channels)%0x90==0,f'{name} channel bytes are not 0x90-aligned')
            vals=[]
            for off in range(0,len(channels),0x90): vals.extend(struct.unpack_from('<4I',channels,off+0x20))
            diff(f'actor[{key}].{name}.channels',vals,nm['channels'])
            compared['channel_words'] += len(vals)
            context=orig.get('context_id')
            if context==0xffff: diff(f'actor[{key}].{name}.has_motion',False,nm.get('has_motion'))
            else:
                compared['motion_contexts'] += 1
                diff(f'actor[{key}].{name}.has_motion',True,nm.get('has_motion'))
                diff(f'actor[{key}].{name}.pc',orig.get('motion_pc_minus_base'),nm.get('pc'))
                ctx=raw(orig['context'],f'{name} context',24)
                compared['bytes'] += 4
                diff(f'actor[{key}].{name}.script_state',u32(ctx,0),nm.get('script_state'))
    # Registrations are paired in source; native entity order follows the table.
    native_entities=native.get('entities'); need(isinstance(native_entities,list),'native entities must be array')
    expected_reg=sorted(regs)
    if len(native_entities)!=len(expected_reg): diff('entities.count',len(expected_reg),len(native_entities))
    for ix,(ri,rp) in enumerate(sorted(regs.items())):
        if ix>=len(native_entities): continue
        ent=native_entities[ix]; orig=rp['complete'];
        diff(f'entity[{ix}].kind',orig.get('registration_kind'),ent.get('kind'))
        cid=orig.get('constructor_id'); key=ctor_map.get(cid) if cid is not None else None
        expected_id=None
        if key is not None:
            original_child=raw(ctors[cid]['complete']['child'],'registration constructor child')
            expected_id=u16(original_child,0x130)
        diff(f'entity[{ix}].actor',expected_id,ent.get('actor'))
        size=0 if key is None else (0x50 if key[0]=='party' else 0x6c)
        diff(f'entity[{ix}].record_bytes',size,ent.get('record_bytes'))
        compared['registrations'] += 1
    rng_by_ctor={}
    for row in rng:
        if row.get('ra') not in RNG_CALLERS:
            unsupported.append(f"RNG row {row['sequence']} return address {row.get('ra')} is outside identified return sites")
        state=row.get('rng_state'); need(isinstance(state,int),'RNG state missing')
        rng_by_ctor.setdefault(row.get('constructor_id'),[]).append({'ra':row['ra'],'output':(state*0x41c64e6d+0x3039)&0x7fffffff})
    rng_report=[]
    for cid,key in ctor_map.items():
        original=rng_by_ctor.get(cid,[]); actor=native_by_key.get(key)
        actual=(actor or {}).get('random_draws',[])
        entry={'constructor_id':cid,'identity':list(key),'original_return_addresses':[x['ra'] for x in original],
               'original_outputs':[x['output'] for x in original],'native_draws':actual,'lengths_match':len(original)==len(actual)}
        if len(original)==len(actual):
            entry['sequence_match']= [x['output'] for x in original]==actual
            if not entry['sequence_match']:
                diffs.append({'path':f'rng.constructor[{cid}]','expected':[x['output'] for x in original],'actual':actual,'reason':'matching observation lengths but outputs differ'})
        else:
            entry['sequence_match']=None
            unsupported.append(f'constructor {cid} RNG sequence lengths differ ({len(original)} original in-flight observations, {len(actual)} native draws); scheduled-work interleaving makes sequence comparison ambiguous')
        rng_report.append(entry)
    unsupported.extend(['RNG exclusivity cannot be proven from in-flight entry observations alone', 'visibility and render flags use sourced original fields and masks; native full constructor flags are not equated to the original masked field'])
    return {'schema':'okage-battle-construction-comparison-v1','reference_schema':SCHEMA,
      'native_schema':native.get('schema'),'reference_frame_convention':reference[0].get('frame_convention'),
      'native_frame':native.get('frame'),'frame_alignment':'No frame shifting; reference vblank and native frame are reported as separate anchors.',
      'complete':bool(native.get('complete')),'native_error':native.get('error',''),
      'source_assertions':[{'field':'each present instance_record+0x32','expected':0,'result':'checked per instance; nonzero values are differences'}],
      'compared':compared,
      'counts':{'original_actors':len(expected),'native_actors':len(actors),'missing_actors':len(missing),'unexpected_actors':len(unexpected),'original_registrations':len(regs),'native_entities':len(native_entities),'differences':len(diffs),'unsupported_checks':len(unsupported)},
      'missing_actors':[list(x) for x in missing],'unexpected_actors':[list(x) for x in unexpected],
      'registration_count':{'original':len(regs),'native':len(native_entities)},'rng':{'original_observed_entries':len(rng),'native_draws':sum(len(a.get('random_draws',[])) for a in actors),'known_return_addresses':sorted(RNG_CALLERS),'constructors':rng_report},
      'first_difference':diffs[0] if diffs else None,'differences':diffs,'unsupported_checks':unsupported}

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--reference',required=True); ap.add_argument('--native',required=True); ap.add_argument('--output',required=True); args=ap.parse_args()
    out=Path(args.output)
    if out.exists() or out.is_symlink(): print(f'refusing existing output: {out}',file=sys.stderr); return 2
    try:
        ref=load_ref(args.reference); native=json.loads(Path(args.native).read_text()); result=compare(ref,native)
        out.parent.mkdir(parents=True,exist_ok=True); out.write_text(json.dumps(result,indent=2)+'\n')
        print(json.dumps(result['counts'],sort_keys=True)); return 0 if not result['differences'] and result['complete'] else 1
    except (OSError,Invalid,json.JSONDecodeError,KeyError,TypeError,ValueError,struct.error) as e:
        print(f'comparison input error: {e}',file=sys.stderr); return 2
if __name__=='__main__': raise SystemExit(main())
