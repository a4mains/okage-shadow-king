#!/bin/zsh
set -e
old=$1 new=$2 stage=$3 out=$4 frames=$5
[[ -e $out ]] && { echo "output must be fresh: $out" >&2; exit 2; }
mkdir -p $out
shasum -a 256 $old $new > $out/binaries.sha256
git -C ${0:A:h}/.. rev-parse HEAD > $out/source-head.txt
cp $stage/.owned-game-data-manifest.json $out/stage-manifest.json 2>/dev/null || true
for tag in old new; do
  bin=${(P)tag}
  for run in a b; do
    d=$out/$tag-$run; mkdir -p $d
    cmd=($bin --game-data $stage --save-directory $d/saves --frames $frames --capture $d/f.png)
    print -r -- "${cmd[@]}" > $d/command.txt
    "${cmd[@]}" > $d/stdout.txt 2>&1; print $? > $d/exit.txt
    (cd $d && shasum -a 256 f.png* > outputs.sha256)
  done
done
