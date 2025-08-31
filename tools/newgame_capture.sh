#!/bin/sh
ORACLE=$1; DISC=$2; ELF=$3; EV=$4
[ -x "$ORACLE" ] || { echo "oracle not executable" >&2; exit 2; }
[ -n "$EV" ] || { echo "usage: $0 ORACLE DISC ELF EVIDENCE_DIR [RUN_SPEC ...]" >&2; exit 2; }
shift 4
if [ $# -eq 0 ]; then
  set -- fill00-a:00: fill00-b:00: fillff-a:ff: fillff-b:ff: control-minimal-fill00:00:--newgame-chapter+--minimal
fi
if [ -n "$OKAGE_PLAY_VM_LOCK" ]; then
  until mkdir "$OKAGE_PLAY_VM_LOCK" 2>/dev/null; do echo "VM lock busy, retrying in 30 s" >&2; sleep 30; done
  trap 'rmdir "$OKAGE_PLAY_VM_LOCK"' EXIT
  trap 'exit 130' INT TERM
fi
status=0
for spec in "$@"; do
  name=${spec%%:*}; rest=${spec#*:}; fill=${rest%%:*}; mode=${rest#*:}
  [ -n "$mode" ] || mode=--newgame-chapter
  words=$(echo "$mode" | tr '+' ' ')
  [ "$mode" != default ] || words=
  out="$EV/runs/$name"; cwd="$EV/cwd/$name"
  [ ! -e "$out" ] || { echo "$out exists" >&2; exit 2; }
  [ ! -e "$cwd/local" ] || { echo "$cwd is not fresh" >&2; exit 2; }
  mkdir -p "$EV/runs" "$cwd"
  echo "== $name (fill $fill $words)"
  if ( cd "$cwd" && "$ORACLE" "$DISC" "$ELF" "$out" "$fill" --deadline-seconds "${OKAGE_NEWGAME_DEADLINE:-3600}" $words ) \
       > "$out.log" 2>&1; then tail -1 "$out.log"; else echo "FAILED (retained): $out.log" >&2; tail -3 "$out.log" >&2; status=1; fi
done
exit $status
