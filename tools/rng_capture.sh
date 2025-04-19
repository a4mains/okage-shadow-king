#!/bin/sh
set -e
ORACLE=$1; DISC=$2; ELF=$3; EV=$4
[ -x "$ORACLE" ] || { echo "oracle not executable" >&2; exit 2; }
for name in rng-a rng-b; do
  [ ! -e "$EV/runs/$name" ] || { echo "$EV/runs/$name exists" >&2; exit 2; }
  [ ! -e "$EV/cwd/$name/local" ] || { echo "$EV/cwd/$name is not fresh" >&2; exit 2; }
  mkdir -p "$EV/runs" "$EV/cwd/$name"
  echo "== $name"
  ( cd "$EV/cwd/$name" && "$ORACLE" "$DISC" "$ELF" "$EV/runs/$name" 00 --deadline-seconds 1500 --rng-products ) \
    > "$EV/runs/$name.log" 2>&1
  tail -1 "$EV/runs/$name.log"
done
