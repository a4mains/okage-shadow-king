#!/bin/sh
set -e
ORACLE=$1; DISC=$2; ELF=$3; OUT=$4
[ -x "$ORACLE" ] || { echo "oracle not executable" >&2; exit 2; }
mkdir -p "$OUT"
for spec in fill00-a:00: fill00-b:00: fillff-a:ff: fillff-b:ff: control-minimal-fill00:00:--minimal; do
  name=${spec%%:*}; rest=${spec#*:}; fill=${rest%%:*}; extra=${rest#*:}
  echo "== $name (fill $fill $extra)"
  if [ -n "$extra" ]; then
    "$ORACLE" "$DISC" "$ELF" "$OUT/$name" "$fill" --deadline-seconds 1500 "$extra" > "$OUT/$name.log" 2>&1
  else
    "$ORACLE" "$DISC" "$ELF" "$OUT/$name" "$fill" --deadline-seconds 1500 > "$OUT/$name.log" 2>&1
  fi
  tail -1 "$OUT/$name.log"
done
