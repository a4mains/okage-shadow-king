#!/bin/zsh
set -u
root=${0:A:h:h}; cd $root
V=${NEWGAME_VALIDATION:?set NEWGAME_VALIDATION=local/research/newgame-validation-vN}; out=$V/$1
[[ -e $out ]] && { echo "fresh output required: $out" >&2; exit 2; }
mkdir -p $out
STAGE=local/game-data-stage-v13
SHA=1696227bf62b137522e12ea2f7529a0138f4ffeea449987901a140b6b57d7ad6
ORIG=local/research/newgame-original-v1/analysis
ORIG_RETIRE=local/research/retire-title-original-v1/analysis
ORIG_RESTORE=local/research/restore-original-v1/analysis
LIGHT_RESTORE=local/research/restore-original-v1/light/analysis
NEW=$V/bin/new-release.app/Contents/MacOS/okage-native
SAN=$V/bin/new-sanitize.app/Contents/MacOS/okage-native
OLD=$V/bin/old-restore.app/Contents/MacOS/okage-native
TITLE_NG=tests/data/retire-routes/newgame-scripted/title-pad.txt
(cd $V/bin && shasum -a 256 -c binaries.sha256) > $out/binaries-check.txt 2>&1 || { echo "pinned binaries changed" >&2; exit 2; }
git rev-parse HEAD > $out/source-head.txt
print -r -- "NEWGAME_VALIDATION=$V tools/newgame_validation_run.sh $1" > $out/driver-command.txt
python3 tools/retire_input_provenance.py audit $STAGE --manifest-sha256 $SHA --out $out/stage-audit-current-time.json > $out/stage-audit.txt 2>&1
echo "stage-audit exit=$?" >> $out/stage-audit.txt
log() { print -r -- "$*" >> $out/commands.txt; }
reads() {  # run directory
  python3 tools/retire_input_provenance.py check-reads $STAGE $1/reads.jsonl --manifest-sha256 $SHA --strict-stage --out $1/reads-check.json > $1/reads-check.txt 2>&1; print $? > $1/reads-check.exit
}
session() {
  local tag=$1 bin=$2 kind=$3 d=$out/sessions/$1-$3 frames=1400
  mkdir -p $d/saves
  local cmd=($bin --game-data $STAGE --save-directory $d/saves --input-log $d/reads.jsonl --newgame-report $d/report)
  case $kind in
    scripted) cmd+=(--title-pad-script $TITLE_NG --name-pad-script tests/data/newgame-routes/newgame-chapter/name-pad.txt);;
    no-then-yes) cmd+=(--title-pad-script $TITLE_NG --name-pad-script tests/data/newgame-routes/no-then-yes/name-pad.txt);;
    fixed) cmd+=(--buttons tests/data/newgame-routes/newgame-fixed/buttons.txt --directions tests/data/newgame-routes/newgame-fixed/directions.txt); frames=1600;;
    continue) cmd+=(--title-pad-script $TITLE_NG --name-pad-script tests/data/newgame-routes/newgame-chapter/name-pad.txt); frames=2400;;
  esac
  cmd+=(--frames $frames --capture $d/f.png)
  log "${cmd[@]}"; print -r -- "${cmd[@]}" > $d/command.txt
  ls -A $d/saves > $d/saves-before.txt
  "${cmd[@]}" > $d/stdout.txt 2>&1; print $? > $d/exit.txt
  ls -A $d/saves > $d/saves-after.txt
  reads $d
  python3 -I tools/newgame_compare.py $ORIG $d/report --strict --json $d/compare.json > $d/compare.txt 2>&1; print $? > $d/compare.exit
  (cd $d && shasum -a 256 f.png* > outputs.sha256)
}
for tag bin in release $NEW sanitize $SAN; do for kind in scripted no-then-yes fixed; do session $tag $bin $kind; done; done
session release $NEW continue
for tag bin in release $NEW sanitize $SAN; do
  d=$out/restore-$tag-scripted; mkdir -p $d/saves
  cmd=($bin --game-data $STAGE --save-directory $d/saves --input-log $d/reads.jsonl --restore-cancel-report $d/report --title-pad-script tests/data/retire-routes/restore-scripted/title-pad.txt --restore-cancel-after-ready 1 --frames 1300 --capture $d/f.png)
  log "${cmd[@]}"; "${cmd[@]}" > $d/stdout.txt 2>&1; print $? > $d/exit.txt; ls -A $d/saves > $d/saves-after.txt; reads $d
  python3 -I tools/restore_light_compare.py $LIGHT_RESTORE $d/report --strict --json $d/light-compare.json > $d/light-compare.txt 2>&1; print $? > $d/light-compare.exit
  python3 -I tools/restore_compare.py $ORIG_RESTORE $d/report --json $d/compare.json > $d/compare.txt 2>&1; print $? > $d/compare.exit
  python3 tools/restore_io_timing_evidence.py $d/report --require-exact --json $d/io-timing.json > $d/io-timing.txt 2>&1; print $? > $d/io-timing.exit
  d=$out/restore-$tag-pending; mkdir -p $d/saves
  cmd=($bin --game-data $STAGE --save-directory $d/saves --restore-cancel-report $d/report --buttons tests/data/restore-routes/pending-cancel/buttons.txt --restore-storage-latency-ms 3000 --frames 1300 --capture $d/f.png)
  log "${cmd[@]}"; "${cmd[@]}" > $d/stdout.txt 2>&1; print $? > $d/exit.txt; ls -A $d/saves > $d/saves-after.txt
done
# Retire routes (strict, original title-relative pad scripts) on the new binary.
for route in restore newgame; do
  d=$out/retire-$route; mkdir -p $d/saves
  cmd=($NEW --game-data $STAGE --save-directory $d/saves --title-pad-script tests/data/retire-routes/$route-scripted/title-pad.txt --title-retirement-report $d/report --frames 1200 --capture $d/f.png)
  log "${cmd[@]}"; "${cmd[@]}" > $d/stdout.txt 2>&1; print $? > $d/exit.txt
  python3 -I tools/retire_title_compare.py $ORIG_RETIRE $d/report --strict > $d/compare.txt 2>&1; print $? > $d/compare.exit
done
# Startup/RNG/Title gates (700-frame cold Session).
for tag bin in release $NEW sanitize $SAN; do
  d=$out/gates-$tag; mkdir -p $d/saves
  cmd=($bin --game-data $STAGE --save-directory $d/saves --startup-texture-report $d/startup-report --title-resource-report $d/title-report --frames 700 --capture $d/f.png)
  log "${cmd[@]}"; "${cmd[@]}" > $d/stdout.txt 2>&1; print $? > $d/exit.txt
  python3 tools/rng_compare.py local/research/rng-original-v1/analysis $d/startup-report > $d/rng.txt 2>&1; print $? >> $d/rng.txt
  python3 tools/rng_startup_timeline_compare.py local/research/startup-original-v1/runs/fill00-a/events.jsonl $d/startup-report > $d/timeline.txt 2>&1; print $? >> $d/timeline.txt
  python3 tools/startup_compare.py local/research/startup-original-v1/analysis $d/startup-report > $d/startup.txt 2>&1; print $? >> $d/startup.txt
  python3 tools/title_compare.py local/research/title-original-v1/analysis $d/title-report > $d/title.txt 2>&1; print $? >> $d/title.txt
done
pairs() {  # dir tool route-args...
  local r=$1 tool=$2; shift 2; : > $r/compare.txt
  for a in $r/old-? $r/new-?; do for b in $r/old-? $r/new-?; do [[ $a < $b ]] || continue
    echo "== ${a:t} ${b:t}" >> $r/compare.txt; python3 $tool $a $b "$@" --json $r/${a:t}-${b:t}.json >> $r/compare.txt 2>&1; echo "exit=$?" >> $r/compare.txt; done; done
}
log tools/newgame_route_regression.sh $OLD $NEW $STAGE $SHA tests/data/newgame-routes/newgame-fixed $out/regression-newgame 1500 4
tools/newgame_route_regression.sh $OLD $NEW $STAGE $SHA tests/data/newgame-routes/newgame-fixed $out/regression-newgame 1500 4 > $out/regression-newgame.log 2>&1
pairs $out/regression-newgame tools/newgame_route_compare.py
log tools/retire_route_regression.sh $OLD $NEW $STAGE $SHA tests/data/restore-routes/restore-cancel $out/regression-restore-cancel 1300 4
tools/retire_route_regression.sh $OLD $NEW $STAGE $SHA tests/data/restore-routes/restore-cancel $out/regression-restore-cancel 1300 4 > $out/regression-restore-cancel.log 2>&1
pairs $out/regression-restore-cancel tools/retire_route_compare.py --route restore-cancel
log tools/rng_cold_start_regression.sh $OLD $NEW $STAGE $out/regression-cold 1800
tools/rng_cold_start_regression.sh $OLD $NEW $STAGE $out/regression-cold 1800 > $out/regression-cold.log 2>&1
r=$out/regression-cold; : > $r/align.txt
for p in "old-a old-b" "new-a new-b" "old-a new-a" "old-b new-b" "old-a new-b" "old-b new-a"; do set -- ${=p}
  echo "== $1 $2" >> $r/align.txt; python3 tools/title_regression_align.py $r/$1 $r/$2 >> $r/align.txt 2>&1; echo "exit=$?" >> $r/align.txt; done
echo done > $out/DONE
