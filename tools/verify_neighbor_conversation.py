#!/usr/bin/env python3
"""Audit Annie's complete window interval using five completed original captures.

Compares1170 controller returns, subsequent1050 choice records and pressed-input
latches. Also checks the original event handoff and optional native movement.
No general input-held-duration, full-scene, audio or pixel equivalence is claimed.
"""
import argparse
import json
from pathlib import Path
from verify_event import write_exact
from verify_message_opening import rows,require,digest,compare_controller
from verify_interaction_scene import actor_rows

# Prefix, first original frame, native anchor, observed frames, controller/choice hashes.
INTERVALS=[
 ('outdoor-neighbor-251-v2',131,5730,120,'408465d3fbbf3c0ce95d8eb051158472c1fb42f41aed052aef0c4fe0075902c1','81306cdfb8540c198c9b5b3a35eb835c5b7c11fae1f3242dfdd9888b350c6f7f'),
 ('neighbor-choice-300-v1',0,5850,300,'1511c1654364e11bea3152d8a8d6cbd1f85a21908d26f72705cd55df30bfbee2','2f91e510141484a43fd5424dcf38ea90dcf9b4d20b4b0c4db2d083c27aa3a18f'),
 ('neighbor-answer-240-v1',0,6150,240,'cef9b750c5a40aa29884fef76dfc416b04c4a820c1185df20c2c3851c7df8d71','f00fc8474fb8f097d20f165dede7c8ea257b1993b349d2526e34bcbb51af8546'),
 ('neighbor-second-choice-300-v1',0,6390,300,'49aed5efc7486bcc2a86d32f4ff24100c488f073ce34b34112d7cf4649027451','290232266c025ef06854c95cd07145dc2717ef7003a933b515ae72dc234e9e21'),
 ('neighbor-close-240-v1',0,6690,210,'41a98684301368433da69e446684162453ef189973cd7ec39cbc46206b0a888b','4c6326ebac305c5e31326c01aaba1230eda694e78312b822eabb04817f950eee')]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--capture',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();base=str(args.capture)
    dialogue=Path(base+'.dialogue.bin');choices=Path(base+'.choices.bin')
    native=dialogue.read_bytes();choice_rows=rows(choices.read_bytes(),12);total=len(choice_rows)
    require(total>=6929,'Native capture does not cover the complete window interval')
    require([r[0] for r in choice_rows]==list(range(1,total+1)),'Native choice frame identity')
    run=json.loads(Path(base.removesuffix('.png')+'.run.json').read_text())
    require(run['exit_code']==0,'Incomplete native conversation run')
    report=dict(scope=__doc__,native_run=run,intervals=[],sha256={str(p):digest(p) for p in [args.capture,dialogue,choices]})
    for index,(name,first,anchor,count,controller_sha,choice_sha) in enumerate(INTERVALS):
        ref=Path('local/reference-'+name+'.controller.bin');options=Path(str(ref)+'.choices.bin')
        require(digest(ref)==controller_sha and digest(options)==choice_sha,'Changed original conversation corpus')
        original_run=json.loads(Path('local/reference-'+name+'.run.json').read_text())
        require(original_run['exit_code']==0,'Incomplete original conversation interval')
        words=compare_controller(ref.read_bytes(),native,first_original=first,first_native=anchor,frames=count,total=total)
        ref_options=rows(options.read_bytes(),16)
        require(len(ref_options)==2*count,'Choice interval extent')
        for i,r in enumerate(rows(ref.read_bytes(),64)[::2]):
            require(choice_rows[anchor-1+i][9]==r[9],f'Pressed input differs at native{anchor+i}')
        if index:
            for i,r in enumerate(ref_options[1::2]):
                require(r[2:6]==choice_rows[anchor-1+i][2:6],f'Choice state differs at native{anchor+i}')
        report['intervals'].append(dict(reference=name,first_original=first,first_native=anchor,frames=count,
                                       controller_words=words,choice_words=count*4 if index else 0,original_run=original_run))
        for path in [ref,options]:report['sha256'][str(path)]=digest(path)
    states=rows(native,12)
    require(states[6897][10]==9 and states[6864][10]==6,'Window closure stages differ')
    require(choice_rows[6159][5]==0 and choice_rows[6700][5]==0,'Reviewed answers must both select option0')
    report.update(controller_frames=1170,controller_words=11700,choice_words=4200,pressed_latches=1170,
                  closure_begin_frame=6865,closure_finished_frame=6898)
    if total>=7229:
        reference=Path('local/reference-neighbor-handoff-300-v1.interaction.bin')
        require(digest(reference)=='74dbf1169a3168e969c4a89357883295c0dae76c3331e0b6a145bbb6d26b438f','Changed original handoff')
        original_run=json.loads(Path('local/reference-neighbor-handoff-300-v1.run.json').read_text())
        require(original_run['exit_code']==0,'Incomplete original handoff run')
        interaction=Path(base+'.interaction.bin');hero_path=Path(base+'.actor-1.bin')
        original=rows(reference.read_bytes(),176);live=rows(interaction.read_bytes(),8);hero=actor_rows(hero_path)
        require(len(live)==len(hero)==total,'Native handoff trace extent')
        require(all(r[0]==i+1 for source in [live,hero] for i,r in enumerate(source)),'Native handoff frame identity')
        manager=[r for r in original if r[1]==0x133a98]
        require([r[0] for r in manager]==list(range(300)),'Handoff manager interval')
        require([(r[0],r[9],r[15]) for r in original if r[1]==0x132ca8]==[(122,5,1)],'Original handoff callback changed')
        for r in manager:
            n=live[6930+r[0]-2]
            require((n[4],n[7],n[5],n[6])==(int(bool(r[20]&0x100)),r[21],r[15],r[150]),
                    f'Event handoff state differs at original{r[0]}')
        require(live[7050][4:7]==(0,1,1) and live[7051][4:7]==(0,0,1),'Native handoff timing differs')
        require(hero[7228][12:16]==manager[-1][28:32],'HERO endpoint differs')
        report['handoff']=dict(original_run=original_run,manager_words=1200,hero_endpoint_words=4,
                               script_finished_frame=7051,control_return_frame=7052)
        for path in [reference,interaction,hero_path]:report['sha256'][str(path)]=digest(path)
        if total>=7300:
            movement_path=Path(base+'.movement.bin');camera_path=Path(base+'.camera.bin')
            movement=rows(movement_path.read_bytes(),16);camera=rows(camera_path.read_bytes(),16)
            require(len(movement)==len(camera)==total,'Native movement/camera extent')
            require(all(r[0]==i+1 for source in [movement,camera] for i,r in enumerate(source)),'Native movement frame identity')
            require(all(r[1]==0x4000 and r[2]==0 for r in movement[7229:7249]),'Post-conversation movement input/gate')
            require(hero[7228][12:16]!=hero[7249][12:16],'HERO did not move after conversation')
            require(all(r[12:16]==hero[7299][12:16] for r in hero[7250:7300]),'HERO did not stop after input release')
            require(all(r[4]==0 and r[5]==0 for r in live[7229:7300]),'Movement started an unexpected event or stayed gated')
            require(camera[7228][4:7]!=camera[7249][4:7],'Exploration camera did not follow movement')
            report['native_movement']=dict(start=7230,release=7250,end=7300,moved=True,stopped=True,camera_followed=True,
                                           scope='Native behavior only; no original post-handoff movement comparison')
            for path in [movement_path,camera_path]:report['sha256'][str(path)]=digest(path)
    write_exact(args.output,(json.dumps(report,indent=2)+'\n').encode())
    print('1170 controller frames,1050 choice records and window closure match')

if __name__=='__main__':main()
