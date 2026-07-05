## CURRENT: texture-to-Chapter-2 completion review v2

pushed). Start with the

the results are:
- The unchanged historical prefix's inputs and the 115 requested status rows are
- Four natural Chapter 2 battles are won.
- A new bedroom Save is written (`f94fa5a2…`). Another process restores it cold
  and play continues.
- A second Restore process ran the music variant live (`variant=1`, three

Open limits:
- Candidate selection remains host-load dependent (17 rejected candidates are
  retained).
- The phase-8 roar has not run live on a fixed binary: none of 17 natural
  contacts on it qualified.
- A supplemental dev-route rerun (`claude-final-dev-v2b`) passed through a new
  Save.
- No Session-level message-window test exists.

## Submitted: six-package assignment "demonstrated" (not accepted)

the ledger key `claude_completion_v1` in

## Previous implementation checkpoint: New Game, name entry and Chapter 1

* **Fixes first.** Name entry no longer steps the image counter (`0x337500` advances only through
* **Route.** First cold title → New Game → actual name edit (erase, select, append) and Yes → death/unlink →
* **Original reference.** New observer mode `--newgame-chapter` (unchanged retail, Play, null renderer), 4

## Previous implementation checkpoint: Empty Restore and second-title ownership

* **Fixes first.**
  - **No glyph-cache writer** is reached in the original.
  - **RNG:** every UI layout draw is logged and labelled by call site.
  - **Second title:** built from the live allocator/handle/hash state (records 114..188, A168..A250).
  - ordered lifecycle (1,800 rows, all observed, none owner-asserted);
  - state, effect106 and VRAM;
  - all 13,868 Session RNG draws;
  - all 51 per-traversal manager frames.

  traversal timing differs.
* **Not compared:** images and audio against the original.
* **Fixed along the way:**
  - Restore status text is now empty until 0x212, as in the source.
  - A Cancel while the card scan is pending has a lifetime regression test.

## Previous implementation checkpoint: Retire title updates through title retirement

* Fixes first:
  - Alignment requires complete coverage, with an explicit two-frame transition rule and omission controls.
  - `--input-log` plus `tools/retire_input_provenance.py` check every consumed asset against the pinned stage
    manifest `1696227b…7ad6`.
  - NIGHT_V XIM bytes are shared with the renderer per binding slot. This fixed a real mixed-case reread that
* Original comparison (Play, unchanged retail code, cold boot), Release and ASan+UBSan, both routes:
  - IDENTICAL at first-update, stable-menu, removal-request, resource-retirement and successor-entry: ordered
* Rendered images and audio were not compared with the original (Play's null renderer).
* An independent review found no HIGH issues; its MEDIUM/LOW findings are fixed or scoped in the doc.
* The paused preview, saves and draft worktrees are untouched; runs were headless.

## Previous review checkpoint: Title reviewed; fixes-first Retire assigned

`local/research/title-followup-review-v1`.

## Previous implementation checkpoint: Title persistent ownership through logos and title construction

`docs/native-comparison-pipeline.md`).

What changed:
- **Shape guard fixed first:** EFD template shapes are validated before any index, and the ASan reproducer is
  clean.
- **One Session owner continues past startup:**
  - the first traversal invalidates the colour buffers and depth Z24 lanes, but not the font lanes;
  - logos are appended to the image list and retired in insertion order at resource retirement;
  - the title loads eight SCR images, then NIGHT_V (67 PSM1 names, cache off, class 0), and binds by live
    successor `1cc778`;
  - later history is marked unmodeled without throwing.
- **Source behaviour recovered:** the archive read lower-cases the name buffer in place before `1cc4e0`.

Verification:
- The real headless cold Session (Release and ASan+UBSan) matches the unchanged original at the constructor
- The RNG, timeline and texture checks are preserved.
- The 1,800-frame regression aligns identically on source events, and every run is retained.

Known gaps:
- game over -> title has no runtime replay (Restore -> title does);
- NIGHT_V XIM host decode still reads the archive path;
- draw invalidation is conservative, not a raster;
- Play is not hardware;
- the mode-11 title demo is a pre-existing native limitation.

## Previous implementation checkpoint: RNG pre-scheduler startup

  requests;
- timeline: 151 allocation/upload events with their draw counts;
- texture state: Startup's 79/36 state and every written nibble.

extra window was opened.

## G0 hardware proof accepted; next full-port task T1

functional GPU hardware feasibility.

## Previous implementation checkpoint: Shared texture allocator/address primitives and original LUCAN packets

101058/1b6f50/1b60c0 retire resources.

another native window. Work stays local.

## Previous checkpoint: ordinary natural battle admission; short STG integration replay

short STG `0a125bb`.

**LIVE NEXT ACTION:** session16047 runs

## Previous checkpoint: Chapter 2 runs and returns outdoor control

frames1..21644. App SHA256

Latest genuine checkpoint is

remain required.

## Previous checkpoint: family send-off complete; scene-1 effect update guarded

Runtime SHA256:

18636. Then continue scene 1's normal `e_field` startup event 60. Its direct

400000. Isolated oracle SHA256
the earlier failing village comparison.

## Previous checkpoint: native Rest, healed Save, and cold restart

Release binary SHA256:

## Previous checkpoint: post-chest native Save and cold restart

native behavior.

The new Save is a genuine gameplay

## Previous checkpoint: integrated title, menu input and New Game boundary

109025. Appending Start108900 then Left108990 reaches the real menu and New Game

## Previous checkpoint: combat RNG attribution and native title dependency

- display-105575-v1 exit0,33.657741750s,firstparent104450,intro104752.
- cooperative-105575-v1 exit0,28.475785916s,firstparent104453,intro104755.

## Previous checkpoint: display opportunities restore targeted field return

1. observed-110375-v1,0f566c8,exit0,47.296677625s: same cooperative policy with
2. display-110375-v1,a2a6873,exit1,62.144541333s: real callbacks from104447,
3. return-display-110375-v1,5d17ea4,exit0,62.869237083s: targeted gatefrom108336,

## Previous checkpoint: post-scheduler GPU gate and independent retirement executed

108336. Constructor104452 differs one tick from short run; no favorable rerun or

## Previous checkpoint: cooperative live loader windows integrated; headless startup regression passed

104757. Identical press lists/button prefix precede the first added109837 input.

### Original comparison and remaining fidelity debt

378missingnative,63orderdifferences.

### Implemented and executed

exit0 in32.003730833s. Binary SHA256

### Fixed-anchor comparison

animation implementation was preserved.

### Newly implemented and executed

The old failures remain retained.

### Executed native and original boundaries

104451. Callback reports equal after removing frame only; UI snapshots equal.

### New original evidence and comparison

native runtime.

### Immediate next work

and the existing exports.

need implementation.

overwrite the running bundle.

## Previous checkpoint: native battle UI construction; active callbacks/rendering still guarded

guard. Time 19.832519084s; binary SHA

### Immediate continuation: real party/enemy construction

remain required before combat can run.

### Tests, preview and coordination

separately in the manifest.

### Previous request-only route and retained failure

Previous party-preparation prefix:

### Preview, jobs and workers

One immutable 0947c84 preview runs from

### Preserved fidelity failures

construction or battle readiness.

### New behavior and verified scope

 (137 unique missing frames).

Current Release binary SHA256:

### Native route and original comparisons

Paired bundles under `local/research/`:

- `home1a-exit-pair-v1`: 177 differing frames / 300, no matching tail,
- `field1d-return-pair-v1`: 584 differing frames / 600, 16 matching tail,
- `mysterious-voice-pair-v1`: 15 differing manager frames / 1200,

### Live work and next actions

- Normal loader is0x17d510, using full view-world-bone linear columns; input W
- Deferred normal accumulation is old + M0*x + M1*y + M2*z, in that order.
- Direction matrix is transpose(L) * transpose(view), where L columns are
- Opaque-list membership does not disable blending. Ordinary primitive creation

### Coordination and visible preview

The latest visible preview uses isolated

### Coordination

per-frame image capture is claimed.

### Latest dependency: bound construction and visible block ordering

tasks are complete.

### Latest original field-projection observation

```sh
build/play-research/okage-oracle/okage-boot-oracle \
  'local/disc/Okage - Shadow King (USA).cue' 6 \
  local/reference-home2c-basement-excellent-marks-1200-v1.state.zip \
  --load --projection local/research/shadow-field-state-v1/basement-v1.bin \
  --deadline-seconds 120
```
### Implemented native Metal shadow raster

terminal and polled at this checkpoint.

### Implemented shadow batching

or new route proof is claimed.

### Implemented projection and prism math: reuse exact evidence

all128 outputs byte-identical to v3.

### Implemented native STUN construction and phase0 follow

`{ctest-*-v2,python-tests-v3}.log`.

### Reached service54 dependencies

already exist in `local/research`.

### Remaining limits

when actual progression needs it.

### Earlier implementation checkpoint (36471d7; historical details)

matches all37 captured outputs from

- `...48455-release-v1`: missing SET2 at PC0x848.
- `...48455-release-v2`: unrecovered service0x3e at PC0x854.
- `...48455-release-v3`: after the new handler, still PC0x854,

### Previous summoning checkpoint (b11919b)

- Service0x3e uses owned XYZ (W1), first-frame construction and the shared84-slot
- Templates33/115 constructors and complete element-state lifetimes match the
- `tools/verify_summon_effects.py` passes in Release, Debug and ASan/UBSan:
- SET2 is imported from the owned disc, SHA256
- All19 CTests pass in each build;48 Python tests pass. Existing map25/34 and

### Research and reproduction

check it is still unused):

```sh
python3 local/research/extend-native-road.py \
  local/native-home2c-basement-three-verses-47855-release-v1 \
  local/native-home2c-basement-after-chant-48455-release-v4 \
  48455 --press 47858 \
  --binary build/native-release/okage-native.app/Contents/MacOS/okage-native
```
- Release: `a9487715469ddc713b7b729e3daed18572d2531b85f02b57f6f8f15fffd7e7f6`.
- Debug: `bc745d908f6a2c7e34534e9c07538a0dfdbc5c41187af277628979ecd8624565`.
- ASan/UBSan: `5d397c18142513a4a874d68b4b3d7b563f77c87ec62706c64325cfdf971e6094`.
- Pose oracle: `d622e430fe14a60db9d022b31a0622212c2a6b89b00228e0e94d30dcc7aab84b`.
- Boot oracle unchanged: `6c9a66f5a8cc87dfba486ea24b0168f20cc229d5caa805bdf87aaa9c1f2d84b7`.

## Previous resume point: bakery conversation after corrected entry scheduling

Release runtime SHA256 is

## Previous resume point: village progression; destination first-frame scheduling

11669. Captures `local/native-village-startup-11860-release-v6`, `...-debug-v2`,

12485. All dialogue evidence is currently Release-only, not a pixel/audio claim.

Reference boot oracle SHA256 remains

## Previous resume point: native village dependencies; music-channel control

- 32 sun/cloud world matrices,512 components, exact against two executed original
- Template19 constructor/setter/700 updates:702 records of425 words;70 width/height
- Six VIL message-index/name-bound outputs match original construction/selection.
- Six focused CTests per build and36 Python tests pass. North-gate and boys opening,

save's64MiB `ram` member; SHA256

## Previous resume point: native north gate; destination VIL message bank

**Newest successful native route:**

**Newest completed original save:**

### Validation and reproducible commands

```sh
python3 tools/verify_north_gate.py \
  --capture local/native-north-gate-11460-release-v1.png \
  --output local/UNUSED-NORTH-GATE-AUDIT.json
python3 tools/verify_boys_opening.py \
  --capture local/native-north-gate-11460-release-v1.png --answer --ending \
  --output local/UNUSED-ENDING-REGRESSION.json
```
### Boys' ending alignment

bounded to the150 observed updates.

### Recovered filter contract and research tools

All addresses bind to retail ELF SHA256

## Previous resume point: native road9560; completed original road740

has been used.

| Original relative frame | Native frame for an aligned input | Mask |
| --- | --- | --- |
| 0 | 8261 | 0000 |
| 2 | 8263 | 9000 |
| 102 | 8363 | 0000 |
| 141 | 8402 | 3000 |
| 321 | 8582 | 0000 |
| 341 | 8602 | 1000 |
| 541 | 8802 | 0000 |
| 562 | 8823 | 9000 |
| 641 | 8902 | 0000 |
| 662 | 8923 | 8000 |
| 721 | 8982 | 0000 |

original-relative0 starts at

Original-relative739 ends at
exploratory native9000 ends at
or whole-camera equivalence is asserted.

## Previous checkpoint: system message complete; road traversal

`256f7ac`. Read

Final commands/captures:

## Previous checkpoint: exterior image reached; outdoor movement stops at dust

Final native reports:

- `local/map-geometry-native-final/report.json`
- `local/map-geometry-release-v1/report.json`
- `local/map-geometry-sanitize-v1/report.json`

- 0: position(0,0,10), rotation0, scale1, camera0, identity view.
- 1: position(3,-5,425), rotation(.13,.27,-.11), scale(2,3,.5), camera(.1,.2,.3), identity.
- 2: position(3,-5,460), rotation(.125,.25,.5), scale(.5,2,3), camera(.125,-.25,0), identity.
- 3: original job position/rotation/scale, camera(.07,.63,.1), identity.
- 4: position(3,5,8), rotation(.33,.21,.17), scale(1.5,.25,4), camera(.24,.47,.13);

The fixture is3960 bytes, SHA256
native runtime input.

## Previous checkpoint: map-effect original probes complete; native integration pending

is claimed by this checkpoint.

## Previous checkpoint: exterior setup and two-bank music prerequisites

The new Debug interior regression

## Prompt refresh: interaction entry points clarified

as native interactions.

## Previous research checkpoint: movement contracts recorded

meaning still needs recovery if reached.

unfinished.

## Previous implementation checkpoint: actor collisions and registry pushing validated

```sh
python3 tools/verify_actor_contacts.py --reference local/actor-contact-validation-v1 --output local/actor-contact-next
python3 tools/verify_collision_world.py --reference local/collision-world-validation-v1 --output local/collision-world-next
```
## Previous collision checkpoint: capsule walls and composed map contacts validated

Reproduce with fresh output paths:

```sh
python3 tools/verify_wall_contacts.py --reference local/wall-validation-v1 --output local/wall-next
python3 tools/verify_map_contacts.py --reference local/map-contact-validation-v2 --output local/map-contact-next
```
## Previous integrated checkpoint: native exploration camera; floor/ceiling contacts

input/listening remain unverified.

## Earlier native checkpoint: exclamation bubble and third choice

- Owned TES.EFD and ENV_TEX drive the native type-30 bubble, attachment, animation,
- The 5000-frame native run reaches message 10106's third choice at `0xfdc`,
- Third answers 0 and 2 are accepted in separate 4600-frame captures. Answer 0
- Five symbols × 300 updates plus initial states match original bounded execution:
- The 5000-frame image/state/audio metadata/PCM/traces match across three builds;

## Historical resume update: effect research before integration

- `0x101558` allocates a template instance through `0x101960`, clears flag
- Attachment callback `0x101118` resolves the actor via `0x1bb5b8`, reads its
- `0x1016a8` converts its integer parameter to four floats at offsets
  `0x50..0x5c`; this event passes zero.
- `0x101960` uses 84 active slots and template index `type + 0x54` in the
- `0x101648` requests removal through `0x103640`, which sets flag `0x100`.

```sh
build/play-research/okage-oracle/okage-boot-oracle \
  'local/disc/Okage - Shadow King (USA).cue' 900 \
  local/reference-choice-select-v1.state.zip --load \
  --deadline-seconds 1800 \
  --pad local/reference-second-choice-900-v1.input \
  --save local/reference-second-choice-900-v1.state.zip \
  --capture local/reference-second-choice-900-v1 \
  --controller local/reference-second-choice-900-v1.controller.bin \
  > local/reference-second-choice-900-v1.log 2>&1
```
## Native checkpoint: dialogue choices

- Fourteen branch cases and188 repeat ticks match original EE in all builds.
- Three whole-game choice transitions match11 state/input words each. Original
- The2645-frame AppKit/headless image, PCM and all traces match. The actual view
- The4080-frame native image/PCM/JSON/all traces agree in Debug/Release/ASan/UBSan.
- Recorded captures and exact launch command are in `docs/native-comparison-pipeline.md`;

## Historical resume update: choice research before integration

No native choice changes have been made.

```sh
build/play-research/okage-oracle/okage-boot-oracle \
  'local/disc/Okage - Shadow King (USA).cue' 1450 \
  local/reference-player-1460-v1.state.zip --load \
  --deadline-seconds 2400 \
  --save local/reference-post-narration-2910-v2.state.zip \
  --capture local/reference-post-narration-2910-v2 \
  > local/reference-post-narration-2910-v2.log 2>&1
```
### Recovered choice leads and remaining proof

Do not commit their game text.

- Native `src/native_runtime.mm` guards all but 10100/10101/10107, and
- Message 10102 uses a three-line replacement and `CC` option. Its PME root is
- `0x148240` parses option metadata using `0x19b0e0`: CC is code 1, BC code 2.
- Controller `0x148dc8` case 1 uses global `0x1fcb68` as activation state. The
- `0x19b418` reads current cursor at text-object +0x1e74; `0x19b3e8` reads previous
- `0x19b9c8` collects immediate child nodes. `0x19b448` consults the node at the
- Repeat input getter `0x1c01d0` reads pad state at 0x339592 (stride 0x58); locate its
- Choice presentation is in `0x146ee8`: font 2, three widgets at x164 and y352,

checkpoint and its limitations follow.

## Latest checkpoint: native narration and progression

- RXWS registration/lookup matches 19 original words in Debug/Release/sanitizer.
- All five cues match pinned voice kernels over 280,000 frames each in all builds.
- The 2,340-frame AppKit/headless image, PCM and state/actor/camera/cue traces match.
- At 2340, field waits at 0xe14, message 10107 is closing, flag 2=1, brightness ~0.86.
- Seventeen CTests pass in all builds; twelve Python tests and ordinary-dialogue

checkpoint; recheck before experiments.

## Historical checkpoint: native voices, dry music, device output and fade

- Thirty-two bounded DSP cases match pinned PCSX2; about 71 seconds of dry music
- Original EE fade/request/stop state matches 141 cases /6,752 ticks. Examining
- The 900-frame AppKit run queues 719,200 frames with no reported underruns. PCM,
- All 16 CTest checks pass in the three configurations; twelve Python tests pass.

## Historical checkpoint: native sequencing and note parameters

state matches exactly. Reproduce with:

## Earlier evidence (historical scopes)

- Native XPF decoding matches the original across 741 outer archives, 17,524
- XMD parsing matches original command scanning across 5,578 models, 14,322 blocks
- All 24 HERO motion entries through tick 600 match original channels and facial
- Native Metal renders inspected textured HERO and bag previews. UV/CLAMP outputs
- Native inline XMP parsing and map coordinate/flag conversion match all 265 maps /
  sanitized outputs. See
- Native scene matrix recovery now covers 20,053 instance cases, 20,069 camera
- Native Metal assembles six HOME1C static placements (9,472 triangles, 64 image
- The Play! research VM now renders the title and opening kitchen, accepts recorded
- Raw reference layers are 640x224, before final composition/deinterlace/aspect
- PS2Recomp generated 3,214 candidate functions with 308 indirect warnings; output

## Immediate product milestone

open. Do not call this a playable scene.

the verbatim PCSX2 function at revision

- `e_home1c.xmt` at relative vblank 363, `home1c.xpf` at 374, `a.xmp` at 384.
- STG/XCL at 398; audio requests `evs_msh0.xv`, `evs_msh1.xv`, `evs_msh.xsq`.
- Actors `mam`, `imo`, `stun`, `hero` via model registration `0x1babf8`.
- `0x100b20` coordinates room loading; `0x1bc0b8` loads inline XMP;
- HOME1C has seven definitions, six ordinary placements and 26 handler-1 effects.

## Reference workflow and current states

```sh
# Choose new output names; original captures refuse overwriting.
build/play-research/okage-oracle/okage-boot-oracle \
  'local/disc/Okage - Shadow King (USA).cue' 600 \
  local/reference-opening.state.zip --load \
  --pad config/reference-name-yes.input \
  --save local/reference-next.state.zip --capture local/reference-next \
  --audio local/reference-next.wav --trace-loaders
```
## Native opening-room experiment

```sh
build/native/okage-render-preview --map \
  local/xpf-decoded/FLDDATA/ROOM/HOUSE/HOME1C.XPF/a.xmp \
  local/home1c-next.png local/reference-scene-602-v2/0000
python3 tools/verify_scene.py --output local/scene-validation-v2
```
## Native builds and focused verification

```sh
cmake --build build/native -j 8
ctest --test-dir build/native --output-on-failure
python3 -m unittest discover -s tests -v
ctest --test-dir build/play-research -R okage-input-tests --output-on-failure
build/native/okage-assets map local/xpf-decoded/FLDDATA/ROOM/HOUSE/HOME1C.XPF/a.xmp
python3 tools/verify_map.py
```
```sh
build/native/okage-render-preview local/xpf-decoded/FLDDATA/OBJ/HERO.XPF/a.xmd local/hero-metal-rest.png
build/native/okage-render-preview local/xpf-decoded/FLDDATA/OBJ/HERO.XPF/a.xmd local/hero-metal-motion-3-120.png local/xpf-decoded/FLDDATA/OBJ/HERO.XPF/a.xmt 3 120
```
