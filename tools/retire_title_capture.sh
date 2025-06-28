#!/bin/sh
ORACLE=$1; DISC=$2; ELF=$3; EV=$4
[ -x "$ORACLE" ] || { echo "oracle not executable" >&2; exit 2; }
[ -n "$EV" ] || { echo "usage: $0 ORACLE DISC ELF EVIDENCE_DIR [RUN_SPEC ...]" >&2; exit 2; }
shift 4
if [ $# -eq 0 ]; then
  set -- restore:fill00-a:00: restore:fill00-b:00: restore:fillff-a:ff: restore:fillff-b:ff: \
         restore:control-minimal-fill00:00:--minimal \
         newgame:fill00-a:00: newgame:fill00-b:00: newgame:fillff-a:ff: newgame:fillff-b:ff: \
         newgame:control-minimal-fill00:00:--minimal
fi
if [ -n "$OKAGE_PLAY_VM_LOCK" ]; then
  until mkdir "$OKAGE_PLAY_VM_LOCK" 2>/dev/null; do echo "VM lock busy, retrying in 30 s" >&2; sleep 30; done
  trap 'rmdir "$OKAGE_PLAY_VM_LOCK"' EXIT INT TERM
fi
status=0
for spec in "$@"; do
  route=${spec%%:*}; rest=${spec#*:}; name=${rest%%:*}; rest=${rest#*:}; fill=${rest%%:*}; extra=${rest#*:}
  out="$EV/runs/$route/$name"; cwd="$EV/cwd/$route/$name"
  [ ! -e "$out" ] || { echo "$out exists" >&2; exit 2; }
  [ ! -e "$cwd/local" ] || { echo "$cwd is not fresh" >&2; exit 2; }
  mkdir -p "$EV/runs/$route" "$cwd"
  echo "== $route/$name (fill $fill --title-retire $route $extra)"
  if ( cd "$cwd" && "$ORACLE" "$DISC" "$ELF" "$out" "$fill" --deadline-seconds 3600 --title-retire "$route" $extra ) \
       > "$out.log" 2>&1; then tail -1 "$out.log"; else echo "FAILED (retained): $out.log" >&2; tail -3 "$out.log" >&2; status=1; fi
done
exit $status
