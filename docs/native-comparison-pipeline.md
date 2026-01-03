## Current capture and validation status

AppKit presentation fills4:3.

| Bundle | Fixed native interval | Observed state result | Image/audio result |
| --- | --- | --- | --- |
| `stan-orders-pair-v3` | 75636–76835 | All compared observables agree; full manager/camera/player coverage | Five nominal image pairs; audio captured but unaligned |
| `jam-ready-pair-v1` | 76836–77135 | All compared observables agree; full manager/camera/player coverage | Three nominal image pairs; audio captured but unaligned |
| `jam-talk-pair-v1` | 77136–78335 | All compared observables agree; full manager/camera/player coverage | Six nominal image pairs; audio captured but unaligned |
| `jam-talk-more-pair-v1` | 78336–79535 | All compared observables agree; full manager/camera/player coverage | Six nominal image pairs; audio captured but unaligned |
| `post-jam-stairs-pair-v1` | 79536–79595 | All compared observables agree; full manager/camera/player coverage, no controller samples | Three nominal image pairs; audio pairing rejected on duration |
| `post-jam-village-slope-pair-v1` | 80196–80285 | All compared observables agree; 90/90 manager/camera/player observations, no controller samples | Three diagnostic image pairs; boundary audio extracted with unequal durations and unproven alignment |

Village-slope bundle SHA256:

was substituted.

Reproduce with a fresh output prefix:

```sh
python3 tools/compare_route_audio.py \
  --original-wav local/reference-field1d-post-jam-stairs-60-v1.wav \
  --original-audio-index local/reference-field1d-post-jam-stairs-60-v1.audio-index.json \
  --original-start-vblank 1 --original-end-boundary stop \
  --native-pcm local/native-post-jam-stairs-79595-release-v1.png.audio.pcm \
  --native-rate 48000 --native-start-tick 79536 --native-end-tick 79595 \
  --output local/research/audio-boundaries-v2/next-stairs
```
Bundle SHA256 values:

- `stan-orders-pair-v3/bundle.json`: `147c9e5cbce115cc92a19efa5acd6892fb220532876cb46120ec9d0f4cfc0b59`
- `jam-ready-pair-v1/bundle.json`: `6a976f46fb0357971bf405344f22d5d2730d80eb5b431497adaf77cca86d3f2e`
- `jam-talk-pair-v1/bundle.json`: `7f3387455ceebe630c0c9a4ac1e9e4f77852f42b62f5e73ff923f637e847d925`
- `jam-talk-more-pair-v1/bundle.json`: `ef7daf801b049c9cdbbc64e1a71fe137b9f5b4d9d18bcb12fc07e91ef9d0ae0d`

### Required integration workflow

| Channel | Executed evidence | Remaining work |
| --- | --- | --- |
| State,69456..70655 | Full fixed-anchor audit:1200 differing frames, complete manager/camera/player coverage, no matching tail | Diagnose dialogue/event/camera timing; preserve failures |
| Images,70656..71855 | Five nominal pairs with side-by-side/difference views and metrics | Validate temporal correspondence and address raster differences |
| Audio,70656..71855 | Nominal segments extracted at44.1/48kHz with hashes/statistics | Establish alignment and compatible mixed/dry comparison scope |
| Original capture observer |120-vblank control:85 comparisons match | Bounded observational evidence only; no performance conclusion |
| Native capture observer | All42 endpoint artifacts match control-v2 without selected captures | Bounded run only |
| Interactive performance |20s live CPU profile identifies rounding/render preparation costs | GPU wait, presentation cadence and exact FPS remain unmeasured |

Executed bedroom bundles:

- `local/research/shadow-explanation-pair-v1`: fixed70656 anchor,1198 differing
- `local/research/home2b-wake-pair-v1`: fixed71856 anchor,793 differing frames,
- `local/research/home2b-door-pair-v1`: fixed73056 anchor,75 differing frames,
- `local/research/home2a-stairs-pair-v1`: fixed73296 anchor,76 differing frames,

Original selected frames are in

The original observer control report is

No listening fidelity is claimed.

capture inventory above.

## Existing comparison layers

- `tools/diagnose_home1b_interval.py` compares a complete fixed-anchor interval
  or audio equivalence.
- Subsystem verifiers execute original routines/vector programs and compare
- Native route `.run.json`, state traces, endpoint PNG, PCM and audio/event logs
  separately from original fidelity.

## First-divergence summary

```sh
python3 tools/diagnose_home1b_interval.py \
  --reference local/reference-home2c-basement-excellent-marks-1200-v1 \
  --capture local/native-home2c-basement-excellent-marks-67055-release-v2.png \
  --first-frame 65856 --unblended-target \
  --output local/NEW-excellent-marks-interval.json
python3 tools/summarize_interval_report.py \
  --input local/NEW-excellent-marks-interval.json \
  --output local/NEW-excellent-marks-summary.json
```
### Executed endpoint image comparison

```sh
python3 tools/compare_frame_images.py \
  --original local/reference-home2c-basement-shadow-experts-1200-v1.layer0.png \
  --native local/native-field-shadows-62855-release-v1.png \
  --native-region 0,0,640,224 --output local/NEW-frame-image-pair
```
### Remaining interval coverage

- A manifest identifying source assets, binaries, emulator/backend configuration,
- The complete state audit and its first-divergence summary.
- Corresponding frame images, with side-by-side and difference views.
- Corresponding audio segments, with declared alignment and comparison metrics.
- A passed, failed or not-captured result for each comparison channel. Missing
