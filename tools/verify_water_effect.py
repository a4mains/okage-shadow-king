#!/usr/bin/env python3
"""Verify isolated effect106 state and packed water grids against retained original.

The native executable receives owned EFD/XIM and an explicit placement/RNG fixture;
no captured geometry, RAM or original outputs enter native code. Equality covers
650 updates plus constructor/setter observations, not drawing packets, raster,
scene scheduling or authentic RNG. Clamp/near checks are source assertions,
separate from fields observed in the retained original corpus.
"""
import argparse
import hashlib
import itertools
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = next(p for p in Path(__file__).resolve().parents if (p/'tools/verify_map_effect.py').is_file())
sys.path.insert(0, str(ROOT/'tools'))
from verify_map_effect import compare, ELF_SHA

FRAMES = 650
STEPS = FRAMES + 2
COUNTS = (94,)*16 + (30,)
STATE_WORDS = 73
WATER_WORDS = 5 + 1024*3
REFERENCE_HASHES = {
    '106.input.bin': 'f85938c6811e41079fcc20942a6dad18622084d4f959bc1fccc68a136f2851f6',
    '106.original-v1.bin': '984a65059ff18239d491af72875c0a41fea6b4c7483a6df5861371604d9a9984',
    '106.original-v1.bin.geometry.bin': 'e380d5b861e09483fa6324fbae0fab857f01e1ca7483bb46519e0ffeac3196d2',
    '106.original-v1.bin.uv.bin': '2f571eeabc71ee91dd798fa940f8eea573b877b91edddc9dc8ac9398765030b5',
    '106.original-v1.bin.run.json': 'e6dc70e17bda718e6941cd7140e00f476c73ebd187079102ecce07db20ecae10',
}
EFD_PATH = 'local/effect-input/CMNDATA/2D/TES.EFD'
EFD_SHA = '2c99993d5ac51760c426b4b157fb65c1348c85d09ef2552fc47e5241b3e43c6c'
TEXTURE_PATH = 'local/xpf-decoded/CMNDATA/2D/ENV_TEX.XPF'
TEXTURE_HASHES = {'5sen.xim': 'a6f4aabe8909a195ff37735efaf72001287c37958f5c68fd4d6cd498b42a775b', '6eff01.xim': '57fc8e2e47a985d35ba5531058bd5b490a5d268498122393525f56caa94ac61d', '6eff02.xim': 'f67a1bdd275a54284539246e89cfb2f138bcd9f59c12fb3343867aec5c648d33', 'all.xim': 'e35e6d02436cde092dc26cffd3340cdfa5119173ffe02f080797b288d9fd1947', 'all2.xim': 'af4d452b82ceec1e21a01aa2885aedab6453b281e3005fd44b92c299e5ae0e6a', 'aura.xim': '9b789518dc328b3462e3bc406e66ce61c18d639ae6c4464450e0d79d2daece4a', 'exp9.xim': '0f75f7b7bd0b3c218cdaaeb482bdaafe6ff7e999c6b7d986e1fbdbbc2da3df46', 'exp_c.xim': 'bf48faf54ce36ea1af13844e715c4b0123e713675e2cb5e56610924a3c3cd724', 'f2.xim': 'bea1730e29afffe4f97f9f229fc679f85148e57b91e74a96870c404f93aa7a3e', 'fuki32b.xim': 'f4362dc85edeb174ba4d012d2cad6b666bfef3f420e05366352a8b08306195a4', 'h29_4.xim': 'a683d6bfbe5fef31a398cb762443520271367ef14509dcae0dbdf2d706ae9717', 'heart.xim': '821a7e98c4774c2bc00f8a0786072032bab3967100c3136279797d3506de15a0', 'hit!.xim': '4ca368c7882ba6c67fb31b77fd2abc8ef48c6ca991c93f658a618b9dadabcb26', 'kemuri.xim': 'c04b3c2219b95ce79b524afa5c2d7cf9f363b4d51d6459bfadb00a118c4ccb4a', 'kyusyu16.xim': 'fc34044f98269d70c193a38bdbaad4ceef3e221ce3f351b69d6ff78699cc883a', 'lgtballm4.xim': '81d1ee7bddd13c218cf3a85672e76b5e673c895acf44b1251a52ba64accc7b5b', 'niji.xim': 'b86509274c38f73e3c811402fd43ce1a434a5a9687ff8bc0b00f5a8511eb6b03', 'odoro.xim': '1b19fb7af3dab17670a9873ea0d70902a40e9e2bb374e0d5ebc53f02667b5503', 'oldskilt4.xim': '473ab019281a39cba4b43e1043010d6cf6c02f21af658974902943da392fd6ad', 'onnpu.xim': 'b9294c102f58b85d552145aedada0ffffdad90abffdfb0a14d344ccbb2e58a1c', 'page.xim': '869aae415f2ec8ed6a18b7b874dbf3e5054b57cb1fba7630403da35da5bc641b', 'smoke_4.xim': '52f59a2114feab46c535f56d729e33194b748fbaeb08ce3b8a3c1c29d3629a63', 'spark.xim': 'b8d181a1157a6aaa1ddf7653a959eb82ebfab3dad1fbb6c905ba1b3d22240e24', 'splash_a.xim': 'b133efc1b4a928478d573ea79b3f78db6c0c9298e0d323337aaa01846997d6af', 'suuji.xim': '83e0896ecca63c95ea69858d7e93ce9ada5a5198880ae8e0c29ae2148e9d4fef', 'thunder.xim': '82283a13f1cfaf107d740540f84d8c48b7e3bfb9de7c4a182ae51b8f67d39e80', 'thunderball.xim': 'ec934adf9c3e0fcfa3365e1e1af1f404471d2a6266df582f1d9352ad41b39086', 'treasure.xim': 'e9ccaa79db2df0a8c6b0858ee41415c8b0e81be45c3d034622f5947d51710af1', 'wat_4.xim': '90acd2d41837a60b50773a694fd60c3dfc828b4241dc636d3f8f348d52ad1ae6', 'water.xim': 'a550cb4932f2bc8a5406361c42ec90467b2689d143126e927479338785d0bd3b', 'wind.xim': '16354ff48e492e46dec87ba99d85295c210efe7ea7daac63ec6a0f7423d0eab5'}

def require(ok, message):
    if not ok:
        raise ValueError(message)

def read(path, limit=32*1024*1024):
    path = Path(path)
    require(path.is_file() and path.stat().st_size <= limit, f'Missing/oversized input: {path}')
    data = path.read_bytes()
    require(len(data) <= limit, f'Input exceeded bound: {path}')
    return data

def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as source:
        for block in iter(lambda: source.read(1024*1024), b''):
            h.update(block)
    return h.hexdigest()

def short(word):
    word &= 65535
    return word - 65536 if word & 32768 else word

class WaterMismatch(ValueError):
    def __init__(self, message, **where):
        super().__init__(message)
        self.detail = {'message': message, **where}

def original_records(data, lanes, steps, name):
    at = 0
    for step in range(steps):
        for piece, count in enumerate(COUNTS):
            require(at+16 <= len(data), f'{name}: missing header at step{step} piece{piece}')
            header = struct.unpack_from('<4I', data, at)
            at += 16
            require(header == (step, 0, piece, count), f'{name}: wrong identity/order/count {header}, expected {(step,0,piece,count)}')
            size = count*lanes*4
            require(at+size <= len(data), f'{name}: truncated step{step} piece{piece}')
            words = struct.unpack_from('<'+str(count*lanes)+'I', data, at)
            at += size
            yield step, piece, [words[i:i+lanes] for i in range(0,len(words),lanes)]
    require(at == len(data), f'{name}: trailing records/bytes')

def compare_water(geometry, uv, native, steps=STEPS):
    """Exact observed fields; native row-major identity and source headers checked.

    `steps` exists for small structural self-tests; production uses652 only.
    Original coordinate tags and UV tags are implicit zero in native schema and
    therefore explicitly required zero here. Only original Z-word high16 is out
    of coordinate extent. Native Z high16 must be zero.
    """
    require(isinstance(steps,int) and not isinstance(steps,bool) and 1 <= steps <= STEPS, 'Invalid snapshot count')
    require(len(native) == steps*WATER_WORDS*4, 'Native water extent')
    checked = 0
    coverage = set()
    native_grid = None
    for xyz_record, uv_record in itertools.zip_longest(original_records(geometry,3,steps,'Original XYZ'), original_records(uv,2,steps,'Original UV')):
        require(xyz_record is not None and uv_record is not None, 'Original XYZ/UV record extent differs')
        step,piece,points = xyz_record
        uvstep,uvpiece,coords = uv_record
        require((step,piece,len(points)) == (uvstep,uvpiece,len(coords)), 'Original XYZ/UV record identity differs')
        if piece == 0:
            if step:
                require(len(coverage) == 1024, f'Original grid coverage at step{step-1}')
            coverage = set()
            at = step*WATER_WORDS*4
            header = struct.unpack_from('<5I',native,at)
            expected = (step,32,32,2,0x40400000) if step < 2 else (step,32,32,3,0x41200000)
            if header != expected:
                raise WaterMismatch('Native water identity/dimensions or source-derived clamp/near assertion differs', step=step,
                                    actual=list(header), expected=list(expected),
                                    assertion_scope='First three words schema identity; clamp/near source-derived, not retained-capture comparison')
            native_grid = list(struct.iter_unpack('<3I',native[at+20:at+WATER_WORDS*4]))
            for index,(xy,zword,_) in enumerate(native_grid):
                gx,gy=index%32,index//32
                if (xy&65535,zword) != ((-32+gx*2)&65535,(-32+gy*2)&65535):
                    raise WaterMismatch('Native row-major X/Z identity differs; native Z must be unsigned low16 only',step=step,grid=[gx,gy],actual=[xy&65535,zword],expected=[(-32+gx*2)&65535,(-32+gy*2)&65535])
        for vertex,((xy,rawz,tag),(st,uvtag)) in enumerate(zip(points,coords)):
            x,z=short(xy),short(rawz)
            require(-32 <= x <= 30 and -32 <= z <= 30 and (x+32)%2 == 0 and (z+32)%2 == 0,
                    f'Original coordinate outside fixed grid step{step} piece{piece} vertex{vertex}')
            gx,gy=(x+32)//2,(z+32)//2
            coverage.add((gx,gy))
            if tag or uvtag:
                raise WaterMismatch('Original XYZ/UV tag is not implicit zero',step=step,piece=piece,vertex=vertex,grid=[gx,gy],xyz_tag=tag,uv_tag=uvtag)
            actual=native_grid[gy*32+gx]
            expected=(xy,rawz&65535,st)
            for channel,a,b in zip(('packedXY','unsigned_low16Z','packedUV'),expected,actual):
                if a != b:
                    raise WaterMismatch('Packed grid field differs',step=step,piece=piece,vertex=vertex,grid=[gx,gy],channel=channel,
                                        original_word=f'{a:08x}',native_word=f'{b:08x}')
            checked += 1
    require(len(coverage)==1024, f'Original grid coverage at final step{steps-1}')
    require(checked==steps*1534,'Incomplete packed-point comparison')
    return {'captured_fields_exact':True,'snapshots':steps,'original_piece_records':steps*17,
            'original_packed_points_compared':checked,'native_logical_points':steps*1024,
            'original_duplicate_points_compared':steps*510,'coordinate_and_uv_words_compared':checked*3,
            'original_xyz_and_uv_tags_exact_zero':checked*2,
            'source_header_assertions':{'passed':True,'scope':"Not observed in retained old capture",
                'steps_0_1':{'clamp_mode':2,'near_float_bits':'40400000'},
                'steps_2_onward':{'clamp_mode':3,'near_float_bits':'41200000'}},
            'excluded_original_bits':{'count':checked*16,'per_point':16,'field':'Upper16 of second observer XYZ word only',
                'reason':'Source XYZ point is three int16 coordinates; observer reads adjacent storage in get(point+4).',
                'raw_capture_unchanged':True},'tolerance':None}

def preflight(reference):
    for name,expected in REFERENCE_HASHES.items():
        read(reference/name)
        require(digest(reference/name)==expected, 'Pinned original differs: '+name)
    run=json.loads(read(reference/'106.original-v1.bin.run.json'))
    c=run.get('command',[])
    resolve=lambda p:(ROOT/p).resolve() if not Path(p).is_absolute() else Path(p).resolve()
    require(run.get('exit_code')==0 and len(c)==7 and c[1]=='map-effect-probe' and c[5]==str(FRAMES), 'Original run failed or wrong mode/frame count')
    require(resolve(c[4])==(reference/'106.input.bin').resolve() and resolve(c[6])==(reference/'106.original-v1.bin').resolve(),'Original run fixture/output binding differs')
    require(run.get('input_sha256')==REFERENCE_HASHES['106.input.bin'],'Original manifest input digest differs')
    require(digest(resolve(c[2]))==ELF_SHA and digest(ROOT/'local/extracted/SCUS_971.29')==ELF_SHA,'Different retail ELF')
    require(digest(resolve(c[3]))==run.get('ram_sha256'),'Original prepared-RAM digest differs')
    require(digest(ROOT/EFD_PATH)==EFD_SHA,'Different owned EFD')
    names={p.name for p in (ROOT/TEXTURE_PATH).glob('*.xim')}
    require(names==set(TEXTURE_HASHES),'Owned XIM file set differs')
    for name,expected in TEXTURE_HASHES.items():
        require(digest(ROOT/TEXTURE_PATH/name)==expected,'Different owned XIM: '+name)
    return run

def write_json(path,value):
    with Path(path).open('x') as target:
        json.dump(value,target,indent=2,allow_nan=False)
        target.write('\n')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--native',type=Path,required=True)
    p.add_argument('--reference',type=Path,default=ROOT/'local/research/church-basement-effects-v1')
    p.add_argument('--output',type=Path,required=True,help='Fresh output directory; failed native outputs are retained')
    a=p.parse_args();reference=a.reference.resolve();binary=a.native.resolve()
    original_run=preflight(reference)
    require(binary.is_file(),'Native executable missing')
    binary_hash=digest(binary)
    a.output.mkdir(parents=True,exist_ok=False)
    output=(a.output/'106.native.bin').resolve()
    command=[str(binary),'map-effect-probe',str(ROOT/EFD_PATH),str(ROOT/TEXTURE_PATH),str(reference/'106.input.bin'),str(FRAMES),str(output)]
    report={'scope':__doc__.strip(),'original_run':original_run,'reference_sha256':REFERENCE_HASHES,'elf_sha256':ELF_SHA,
            'owned_efd':{'path':EFD_PATH,'sha256':EFD_SHA},'owned_texture_directory':TEXTURE_PATH,'owned_texture_sha256':TEXTURE_HASHES,
            'source_sha256':{str(f):digest(f) for f in [Path(__file__),ROOT/'tools/verify_map_effect.py',ROOT/'src/effect.cpp',ROOT/'include/okage/effect.hpp',ROOT/'src/assets_main.cpp']},
            'all_captured_fields_exact':False,'checks':{}}
    try:
        result=subprocess.run(command,capture_output=True,cwd=ROOT)
        with (a.output/'106.log').open('xb') as log:log.write(result.stdout+result.stderr)
        run={'command':command,'exit_code':result.returncode,'binary_sha256_before':binary_hash,'binary_sha256_after':digest(binary),
             'input_sha256':REFERENCE_HASHES['106.input.bin']}
    except OSError as exc:
        run={'command':command,'exit_code':None,'binary_sha256_before':binary_hash,'error':str(exc)}
    write_json(a.output/'106.run.json',run);report['native_run']=run
    success=run['exit_code']==0 and run.get('binary_sha256_after')==binary_hash
    if success:
        for name,operation in [('state',lambda:compare(read(reference/'106.original-v1.bin'),read(output),106,1,FRAMES)),
                               ('water',lambda:compare_water(read(reference/'106.original-v1.bin.geometry.bin'),read(reference/'106.original-v1.bin.uv.bin'),read(str(output)+'.water.bin')))]:
            try:
                detail=operation()
                report['checks'][name]={'passed':True,'result':detail}
            except (ValueError,OSError,struct.error) as exc:
                report['checks'][name]={'passed':False,'error':getattr(exc,'detail',{'message':str(exc)})}
        report['all_captured_fields_exact']=all(c['passed'] for c in report['checks'].values())
        if report['checks'].get('state',{}).get('passed'):
            report['checks']['state']['state_words_bitwise_exact']=STEPS*STATE_WORDS
    else:
        report['native_failed_or_binary_changed']=True
    report['native_output_sha256']={str(path):digest(path) for path in (output,Path(str(output)+'.water.bin')) if path.is_file()}
    # An owned asset changing during execution invalidates the run even if bytes compare.
    asset_changes=[name for name,want in TEXTURE_HASHES.items() if digest(ROOT/TEXTURE_PATH/name)!=want]
    if digest(ROOT/EFD_PATH)!=EFD_SHA:asset_changes.append(EFD_PATH)
    report['assets_changed_during_execution']=asset_changes
    report['all_captured_fields_exact'] &= not asset_changes
    write_json(a.output/'report.json',report)
    print(json.dumps({'report':str(a.output/'report.json'),'all_captured_fields_exact':report['all_captured_fields_exact']}))
    return 0 if report['all_captured_fields_exact'] else 1

if __name__=='__main__':
    try:sys.exit(main())
    except (ValueError,OSError,struct.error) as exc:
        print('Invalid water verification input: '+str(exc),file=sys.stderr)
        sys.exit(2)
