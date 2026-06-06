"""Fresh-process Restore with ordinary controls. Uses the retained prelude
tape's title/Restore inputs (Start 809, Confirm 915) and then selects the
requested slot with ordinary Right presses, Confirm, Left (Yes), Confirm.
Captures the activated field.

Requires the run to stop its prelude at the title (frame 808, the frame the
historical Start/Confirm timing is relative to). Success requires the native
log to show the Restore request and an activated field after these inputs,
field control in status, and the restored-field variables capture on disk.
"""
from pathlib import Path

from .outcome import need
from .pilot import CONFIRM, LEFT, RIGHT, START

TITLE_FRAME = 808
RESTORE_REQUESTED = 'Native Restore requested saved field frame='
FIELD_ACTIVATED = 'Native ordinary field activated scene='

def main(c, run, manifest, slot_rights):
    need(manifest['prelude_ticks'] == TITLE_FRAME, 'restore-route',
         f'restore requires a {TITLE_FRAME}-tick prelude', prelude=manifest['prelude_ticks'])
    need(manifest['prepared_requests'] == 0, 'restore-route', 'restore run must not replay prepared requests')
    need(0 <= slot_rights <= 2, 'restore-route', 'slot rights must be 0..2')
    start = c.log_offset()
    s = c.status()
    need(s['frame'] == TITLE_FRAME and s['scene'] is None, 'restore-route', 'not at the title frame', status=s)
    c.step(1, START)
    c.step(105, 0)            # 809 Start, release 810 (tape)
    c.step(1, CONFIRM)
    c.step(285, 0)            # 915 Confirm: Restore, release 916
    c.send('view restore-slots-ready')
    for _ in range(slot_rights):
        c.step(1, RIGHT)
        c.step(20, 0)
    c.send('view restore-slot-chosen')
    c.step(1, CONFIRM)
    c.step(70, 0)
    c.send('view restore-read-prompt')
    c.step(1, LEFT)
    c.step(20, 0)
    c.step(1, CONFIRM)
    for _ in range(300):
        c.step(10, 0)
        s = c.status()
        if s['scene'] is not None:
            break
    c.note('restored field', s)
    need(s['scene'] is not None, 'restore-route', 'no field after Restore', status=s)
    c.step(30, 0)
    c.send('capture restored-field', 'view restored-field-view')
    s = c.status()
    c.note('restored after settle', s)
    text = c.log_since(start)
    need(RESTORE_REQUESTED in text, 'restore-route', 'native log lacks the Restore request')
    need(FIELD_ACTIVATED in text, 'restore-route', 'native log lacks an activated field')
    need(s['hero_position'] is not None and not s['event_running'], 'restore-route', 'no field control after Restore', status=s)
    variables = Path(run) / 'control' / 'restored-field.png.variables.bin'
    need(variables.is_file(), 'restore-route', 'restored-field variables capture missing')
    return {'restored': s, 'variables': str(variables)}
