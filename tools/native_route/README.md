## Commands

```sh
WORK=/path/to/worktree
R=/path/to/research/runs
# prepare + detached launch (the reports are off unless --newgame-report is given)
python3 $WORK/tools/native_route/cli.py launch-new --run $R/strict-x1a \
  --binary $WORK/build/claude-release/okage-native.app/Contents/MacOS/okage-native \
  --stage $WORK/local/game-data-stage-v18 \
  --prepared $R/navigation-pulse-historical-release-v1 \
  --saves $R/navigation-pulse-historical-release-v1/saves \
  --prelude-ticks 21644 --source-repo $WORK [--newgame-report] [--min-free-gib 200]
# strict route: prefix, walk, Rosalyn, contact, battle, home, climb, Save (pinned copy)
python3 $R/strict-x1a/tools/native_route/cli.py strict-full $R/strict-x1a --target-save bkmo2.dat
# fresh Restore run from the Save run's saves (the prelude stops at the title, frame 808)
python3 $WORK/tools/native_route/cli.py launch-new --run $R/restore-x1a \
  --binary ... --stage ... --prepared $R/claude-dev-prepared-v1 \
  --saves $R/strict-x1a/saves --prelude-ticks 808
python3 $R/restore-x1a/tools/native_route/cli.py restore-check $R/restore-x1a \
  --slot-file bkmo2.dat --slot-rights 2 --expect-save-sha256 <save sha256 from strict-x1a/route-result.json>
# strict comparator (inputs/status only)
python3 $WORK/tools/native_route/cli.py compare RUN_A RUN_B [--interval LO:HI | --a-inputs LO:HI ...]
# explicit prelude-candidate search (records PARENT/PREFIX-candidates.json)
python3 $WORK/tools/native_route/cli.py candidates --parent $R --prefix strict-x2 --max 8 \
  --target-save bkmo2.dat --binary ... --stage ... --prepared ... --saves ... --prelude-ticks 21644
```














## Results and exit codes

| code | stage | meaning |
|---|---|---|
| 0 | - | every stage passed its postconditions |
| 2 | prelude-diverged | bedroom exit queued at 2394; native terminated (candidate rejection) |
| 10 | pins | refused: a pin or location check failed |
| 20 | route-prefix | prelude or prepared requests unfinished, or not on the road |
| 21 | route-walk | no Rosalyn event |
| 22 | route-rosalyn | the segment did not leave message 20107 with no event |
| 23 | route-dev-prefix | dev prefix: prelude unfinished, status frame not 808, or not on the road after the retained requests |
| 30 | encounter | no natural contact (budget, field gone, no native contact line) |
| 40 | battle | never constructed, budget exhausted, no native victory, or no field/saved-field return |
| 45 | navigate | the field never settled |
| 50 | home | house interior (scene 70) with field control not reached |
| 60 | home-inside | landing or bedroom startup menu not reached |
| 70 | save | cursor not on Save, or the Save postconditions failed (below) |
| 80 | restore-route | not at title 808, no Restore request or activated field, no variables capture |
| 90 | restore-check | the slot, payload, transition, victory or liveness checks failed |
| 64 | usage | argument error (never 2) |
