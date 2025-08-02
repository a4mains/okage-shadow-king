#!/bin/zsh
set -u
root=${0:A:h:h}; cd $root
V=local/research/restore-ownership-validation-v1; out=$V/$1
[[ -e $out ]] && { echo "fresh output required: $out" >&2; exit 2; }
mkdir -p $out
STAGE=local/game-data-stage-v13
SHA=1696227bf62b137522e12ea2f7529a0138f4ffeea449987901a140b6b57d7ad6
ORIG=local/research/restore-original-v1/analysis
ORIG_RETIRE=local/research/retire-title-original-v1/analysis
NEW=$V/bin/new-release.app/Contents/MacOS/okage-native
SAN=$V/bin/new-sanitize.app/Contents/MacOS/okage-native
OLD=$V/bin/old-retire.app/Contents/MacOS/okage-native
(cd $V/bin && shasum -a 256 -c binaries.sha256) > $out/binaries-check.txt 2>&1 || { echo "pinned binaries changed" >&2; exit 2; }
git rev-parse HEAD > $out/source-head.txt
python3 tools/retire_input_provenance.py audit $STAGE --manifest-sha256 $SHA --out $out/stage-audit-current-time.json > $out/stage-audit.txt 2>&1
echo "stage-audit exit=$?" >> $out/stage-audit.txt
log() { print -r -- "$*" >> $out/commands.txt; }
session() {  # tag binary kind(scripted|fixed|pending)
  local tag=$1 bin=$2 kind=$3 d=$out/sessions/$1-$3
  mkdir -p $d/saves
  local cmd=($bin --game-data $STAGE --save-directory $d/saves --input-log $d/reads.jsonl --restore-cancel-report $d/report)
  case $kind in
    scripted) cmd+=(--title-pad-script tests/data/retire-routes/restore-scripted/title-pad.txt --restore-cancel-after-ready 1);;
    fixed) cmd+=(--buttons tests/data/restore-routes/restore-cancel/buttons.txt);;
    pending) cmd+=(--buttons tests/data/restore-routes/pending-cancel/buttons.txt --restore-storage-latency-ms 3000);;
  esac
  cmd+=(--frames 1300 --capture $d/f.png)
  log "${cmd[@]}"; print -r -- "${cmd[@]}" > $d/command.txt
  ls -A $d/saves > $d/saves-before.txt
  "${cmd[@]}" > $d/stdout.txt 2>&1; print $? > $d/exit.txt
  ls -A $d/saves > $d/saves-after.txt
  python3 tools/retire_input_provenance.py check-reads $STAGE $d/reads.jsonl --manifest-sha256 $SHA --strict-stage --out $d/reads-check.json > $d/reads-check.txt 2>&1; print $? > $d/reads-check.exit
  if [[ $kind != pending ]]; then
    python3 -I tools/restore_compare.py $ORIG $d/report --json $d/compare.json > $d/compare.txt 2>&1; print $? > $d/compare.exit
  fi
}
for tag bin in release $NEW sanitize $SAN; do for kind in scripted fixed pending; do session $tag $bin $kind; done; done
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
log tools/retire_route_regression.sh $OLD $NEW $STAGE $SHA tests/data/restore-routes/restore-cancel $out/regression-restore-cancel 1300 4
tools/retire_route_regression.sh $OLD $NEW $STAGE $SHA tests/data/restore-routes/restore-cancel $out/regression-restore-cancel 1300 4 > $out/regression-restore-cancel.log 2>&1
r=$out/regression-restore-cancel; : > $r/compare.txt
for a in $r/old-? $r/new-?; do for b in $r/old-? $r/new-?; do [[ $a < $b ]] || continue
  echo "== ${a:t} ${b:t}" >> $r/compare.txt; python3 tools/retire_route_compare.py $a $b --route restore-cancel --json $r/${a:t}-${b:t}.json >> $r/compare.txt 2>&1; echo "exit=$?" >> $r/compare.txt; done; done
log tools/rng_cold_start_regression.sh $OLD $NEW $STAGE $out/regression-cold 1800
tools/rng_cold_start_regression.sh $OLD $NEW $STAGE $out/regression-cold 1800 > $out/regression-cold.log 2>&1
r=$out/regression-cold; : > $r/align.txt
for p in "old-a old-b" "new-a new-b" "old-a new-a" "old-b new-b" "old-a new-b" "old-b new-a"; do set -- ${=p}
  echo "== $1 $2" >> $r/align.txt; python3 tools/title_regression_align.py $r/$1 $r/$2 >> $r/align.txt 2>&1; echo "exit=$?" >> $r/align.txt; done
echo done > $out/DONE
