"""Scene 1 (FIELD_1D) walk from Ari's gate (176, 712) to the event-32 Confirm
trigger (x 420..432, z -710..-690) that requests Madril (scene 14 entrance 1);
see the Madril route card.

HOME_LEG is the proven home route (home.WAYPOINTS) reversed, from the gate
to the Rosalyn area (-158, 241); claude-final-hist-v4a/v5d walked it the other
way. ROAD_LEG is a static plan (not evidence) from there to the trigger:
  okage-assets xcl-polygons local/xpf-decoded/FLDDATA/MAP/FIELD/FIELD_1D.XPF/a.xcl
    (a.xcl SHA-256 5b68b79957751c4d066ece44d32c423ad123c747917fb64ff666add10550e7fb;
     polygons JSONL 24cf847056be29bb34016e8506d2bde9028d3fa0360edc3b21cf9c7a3a7d9219)
  field_path.py POLYGONS --start -158 241 --start-y -7.45 --goal 426 -700 --goal-y -23 --goal-event 32
Automatic events crossed by ROAD_LEG: [20, 21, 25, 26, 27, 33, 31] (each ends at the route
card's entry state). c2p-madril-v1a's single whole-route plan left the dirt
path near (55, 793) and stalled against an XCL rock; the planner then gained
wall-crossing checks and this leg split.

ROAD_LEG (from c2p-madril-v1b, rev 2) = the static plan's first 78 points (walked
by v1b to (105, -124)), then the Sonnet operator's validated live trail from
v1b (explore-journal.jsonl): down off the ledge at (128, -119) on lower ground,
over the rope bridge, the Confirm at the bridge-end NPC (FIELD message 20201,
three CC menus, objective 10 -> 60) and on to the trigger. A ('confirm', x, z,
times, objective) entry talks `times` times facing (x, z), running each
event's dialogue, then requires bank-4 objective `objective`. Message 20201
branches on \\o[FFF(486/1/487/1)]: the first talk sets b1[486], the second
b1[487], and only the third plays the CC menus and the objective write
(c2p-madril-v1c talked once and stayed blocked).
"""
from . import home

TRIGGER = (426, -700)
TRIGGER_EVENT = 32
CROSSED = [20, 21, 25, 26, 27, 33, 31]
HOME_LEG = list(reversed(home.WAYPOINTS))
ROAD_LEG = [
    (-158, 241), (-158, 236), (-157, 235), (-157, 227), (-157, 219), (-157, 211),
    (-157, 203), (-157, 199), (-156, 198), (-156, 190), (-156, 182), (-156, 177),
    (-154, 175), (-154, 174), (-149, 169), (-149, 161), (-149, 158), (-144, 153),
    (-139, 148), (-134, 143), (-129, 138), (-124, 133), (-119, 128), (-114, 123),
    (-109, 118), (-104, 113), (-99, 108), (-94, 103), (-89, 98), (-87, 96),
    (-87, 90), (-86, 89), (-86, 88), (-85, 87), (-85, 86), (-81, 82),
    (-81, 74), (-81, 66), (-81, 58), (-81, 50), (-76, 45), (-71, 40),
    (-71, 32), (-71, 30), (-66, 25), (-61, 20), (-56, 15), (-51, 10),
    (-46, 5), (-41, 0), (-36, -5), (-31, -10), (-26, -15), (-21, -20),
    (-16, -25), (-11, -30), (-6, -35), (-1, -40), (4, -45), (9, -50),
    (14, -55), (19, -60), (24, -65), (29, -70), (34, -75), (38, -79),
    (46, -79), (54, -79), (60, -79), (65, -84), (70, -89), (75, -94),
    (80, -99), (85, -104), (90, -109), (95, -114), (100, -119), (105, -124),
    (112, -130), (116.6, -133.7), (124.4, -136.5), (131.3, -132.1), (138, -131), (148.6, -129),
    (164.4, -144.5), (169.8, -161.5), (178.8, -176.5), (180.8, -197.7), (181.5, -222.3), ('confirm', 181.6, -230.0, 3, 60),
    (180, -221.2), (188.8, -242.3), (190.2, -275.8), (189.4, -317.8), (191.5, -357.5), (190.5, -400.3),
    (176.6, -417.2), (157.4, -437.2), (148, -445.5), (129.9, -466.9), (109.9, -486.6), (89.6, -507),
    (73.6, -521), (71.1, -532.5), (69.4, -550.3), (61.8, -568), (60, -571.6), (60.1, -600.6),
    (51.3, -608.6), (48.7, -620.6), (48.1, -627.5), (54.9, -633.3), (57.1, -645.3), (59.4, -652.3),
    (64.8, -653.4), (75.3, -658.7), (89, -664.4), (103.6, -668), (128.6, -666.4), (150.2, -667.5),
    (159.8, -675), (171, -681.6), (180.5, -689.6), (190.1, -697.6), (204.7, -709.6), (216.2, -714.5),
    (223.4, -708.6), (260, -708.7), (298.6, -708.9), (339.8, -709.8), (377.6, -708.5), (400.4, -708.4),
    (410.2, -708), (418.2, -703.3), (426, -700),
]
WAYPOINTS = HOME_LEG + ROAD_LEG[1:]
