#!/bin/zsh
set -e
old=$1 new=$2 stage=$3 manifest_sha=$4 input=$5 out=$6 frames=$7 runs=${8:-2}
[[ -e $out ]] && { echo "output must be fresh: $out" >&2; exit 2; }
actual=$(shasum -a 256 $stage/.owned-game-data-manifest.json | cut -d' ' -f1)
[[ $actual == $manifest_sha ]] || { echo "stage manifest hash $actual != pinned $manifest_sha" >&2; exit 2; }
tools=${0:A:h}
mkdir -p $out/input
cp $input/buttons.txt $out/input/; [[ -f $input/directions.txt ]] && cp $input/directions.txt $out/input/
(cd $out/input && shasum -a 256 * > sha256.txt)
shasum -a 256 $old $new > $out/binaries.sha256
git -C $tools/.. rev-parse HEAD > $out/source-head.txt
cp $stage/.owned-game-data-manifest.json $out/stage-manifest.json
print $manifest_sha > $out/stage-manifest.sha256
for tag in old new; do
  bin=${(P)tag}
  for i in $(seq 1 $runs); do
    run=${${:-abcdefghij}[$i]}
    d=$out/$tag-$run; mkdir -p $d/saves
    cmd=($bin --game-data $stage --save-directory $d/saves --buttons $out/input/buttons.txt)
    [[ -f $out/input/directions.txt ]] && cmd+=(--directions $out/input/directions.txt)
    [[ $tag == new ]] && cmd+=(--input-log $d/reads.jsonl --title-retirement-report $d/report)
    cmd+=(--frames $frames --capture $d/f.png)
    print -r -- "${cmd[@]}" > $d/command.txt
    set +e; "${cmd[@]}" > $d/stdout.txt 2>&1; print $? > $d/exit.txt; set -e
    (cd $d && shasum -a 256 f.png* > outputs.sha256)
    if [[ $tag == new ]]; then
      set +e; python3 $tools/retire_input_provenance.py check-reads $stage $d/reads.jsonl --manifest-sha256 $manifest_sha \
        --out $d/reads-check.json > $d/reads-check.txt 2>&1; print $? > $d/reads-check.exit; set -e
    fi
  done
done
