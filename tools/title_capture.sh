#!/bin/sh
set -e
ORACLE=$1; DISC=$2; ELF=$3; EV=$4
[ -x "$ORACLE" ] || { echo "oracle not executable" >&2; exit 2; }
if [ -n "$OKAGE_PLAY_VM_LOCK" ]; then
  until mkdir "$OKAGE_PLAY_VM_LOCK" 2>/dev/null; do echo "VM lock busy, retrying in 30 s" >&2; sleep 30; done
  trap 'rmdir "$OKAGE_PLAY_VM_LOCK"' EXIT INT TERM
fi
for spec in fill00-a:00: fill00-b:00: fillff-a:ff: fillff-b:ff: control-minimal-fill00:00:--minimal; do
  name=${spec%%:*}; rest=${spec#*:}; fill=${rest%%:*}; extra=${rest#*:}
  [ ! -e "$EV/runs/$name" ] || { echo "$EV/runs/$name exists" >&2; exit 2; }
  [ ! -e "$EV/cwd/$name/local" ] || { echo "$EV/cwd/$name is not fresh" >&2; exit 2; }
  mkdir -p "$EV/runs" "$EV/cwd/$name"
  echo "== $name (fill $fill --title $extra)"
  ( cd "$EV/cwd/$name" && "$ORACLE" "$DISC" "$ELF" "$EV/runs/$name" "$fill" --deadline-seconds 5400 --title $extra ) \
    > "$EV/runs/$name.log" 2>&1
  tail -1 "$EV/runs/$name.log"
done
