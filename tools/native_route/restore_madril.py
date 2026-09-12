"""Cold Restore of the Madril hotel Save and continued play (route card step 3).

In a fresh native process: Restore the pinned hotel Save through the title
menus (restore_route), require the restored variables to equal the saved
payload except playtime, require the boundary state (objective 11, b1[125] 1,
story 7, scene word 107) and the hotel field (scene 107, entrance 0), move,
then continue with ordinary transitions: Confirm on the hotel exit pad
(e_hotel1 event 1 -> scene 14 entrance 10) and back in through the Madril door
(event 17 -> scene 107 entrance 0). The second hotel load is the ordinary
reload that tools/native_route/restore_audit.py compares with the Restore load.
"""
from pathlib import Path

from . import madril, madril_save, restore_check, restore_route, savefmt
from .outcome import need
from .pilot import CONFIRM, RIGHT, hero_moved, in_field_control

EXIT_PAD = (-9.6, -13.5)

def confirm_here(c):
    c.step(2, 0)
    c.step(1, CONFIRM)
    c.step(30, 0)

def main(c, run, manifest, slot_file, slot_rights, expect_sha, record):
    data = restore_check.check_slot(run, manifest, slot_file, expect_sha)
    record('slot', {'slot_file': slot_file, 'expected_save_sha256': expect_sha})
    record('restore-route', restore_route.main(c, run, manifest, slot_rights))
    v = (Path(run) / 'control' / 'restored-field.png.variables.bin').read_bytes()
    diff = restore_check.payload_differences(data, v)
    unexpected = [d for d in diff if int(d['offset'], 16) not in restore_check.ALLOWED_PAYLOAD_DIFFERENCES]
    need(not unexpected, 'restore-check', 'restored variables differ from the saved payload', differences=diff)
    state = madril.progress(v)
    need(state == madril_save.SAVE_STATE, 'restore-check', 'restored state is not the hotel Save state', state=state)
    restored = c.status()
    need(in_field_control(restored, madril_save.HOTEL) and restored['entrance'] == 0, 'restore-check',
         'restored field is not the hotel at entrance 0', status=restored)
    record('payload', {'bytes': savefmt.PAYLOAD_SIZE, 'differences': diff, 'state': state, 'restored': restored})
    c.step(20, RIGHT)
    c.step(1, 0)
    moved = c.status()
    need(hero_moved(restored, moved), 'restore-check', 'ordinary step did not move the restored hero')
    s = c.walk_to(*EXIT_PAD, tol=1.0, max_ticks=300)
    confirm_here(c)
    town = madril_save.wait_scene(c, run, madril_save.MADRIL)
    need(in_field_control(town, madril_save.MADRIL), 'restore-check', 'hotel exit did not reach Madril', status=town)
    c.step(1, 0)
    s = c.walk_to(*madril_save.DOOR, tol=1.0, max_ticks=400)
    need(madril.navigate.near(s, madril_save.DOOR, 2), 'restore-check', 'hotel door pad not reached', status=s)
    confirm_here(c)
    back = madril_save.wait_scene(c, run, madril_save.HOTEL)
    need(in_field_control(back, madril_save.HOTEL), 'restore-check', 'hotel re-entry failed', status=back)
    guards = madril.guard_lines(c.log_since(0))
    need(not guards, 'restore-check', 'guard or error lines in the native log', lines=guards[:20])
    need(c.alive(), 'restore-check', 'native process not alive')
    record('transitions', {'moved': moved, 'madril': town, 'hotel_again': back})
