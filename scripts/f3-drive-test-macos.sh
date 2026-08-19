#!/bin/bash
# macOS `sh` is bash in POSIX mode: $BASH_VERSION is set but process substitution
# is disabled. Re-exec unconditionally once, guarded by a sentinel.
if [ -z "${F3T_REEXEC:-}" ]; then
  F3T_REEXEC=1; export F3T_REEXEC
  exec /bin/bash "$0" "$@"
fi
#
# f3-drive-test-macos.sh — erase, fill, cool down, and repeatedly verify a
# flash drive with f3write/f3read on macOS.
#
# Useful for full-capacity verification of suspect drives, and for
# investigating drives that drop off the USB bus under sustained I/O
# (e.g. thermal problems): run the same drive with and without a cooldown
# between the write and the read passes, and compare.
#
# Usage:  ./f3-drive-test-macos.sh <disk-identifier> <LABEL> [options]
#
#   <disk-identifier>  e.g. disk8   (whole disk, external only; see `diskutil list`)
#   <LABEL>            a name for the physical drive, e.g. DRIVE4
#                      becomes the volume name, the log filename, and is
#                      recorded next to the USB location ID and serial number
#                      so the physical drive, the volume, and the port all
#                      match up in the logs.
#
# Options:
#   --cooldown SEC   idle time between write and read      (default 0)
#   --replug         pause for a physical unplug instead of an idle wait
#   --reads N        consecutive read passes                (default 1)
#   --gb N           limit the test to the first N GB
#   --no-erase       reuse existing .h2w files, skip erase and write
#
# Examples:
#   ./f3-drive-test-macos.sh disk8 DRIVE4
#   ./f3-drive-test-macos.sh disk5 DRIVE3 --no-erase --reads 3
#   ./f3-drive-test-macos.sh disk6 DRIVE2 --cooldown 1800 --replug
#
set -uo pipefail

COOLDOWN=0; READS=1; GB=""; ERASE=1; REPLUG=0

die()  { printf '\033[31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }
info() { printf '\033[36m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[32m%s\033[0m\n' "$*"; }
bad()  { printf '\033[31m%s\033[0m\n' "$*"; }

[ $# -ge 2 ] || die "usage: $0 <disk-identifier> <LABEL> [--cooldown SEC] [--replug] [--reads N] [--gb N] [--no-erase]"

DISK="${1#/dev/}"; NAME="$2"; shift 2

# exFAT volume names via diskutil: keep it short and plain.
printf '%s' "$NAME" | grep -Eq '^[A-Za-z0-9_-]{1,11}$' \
  || die "LABEL must be 1-11 characters, letters/digits/underscore/hyphen only (got '$NAME')."
VOLNAME=$(printf '%s' "$NAME" | tr '[:lower:]' '[:upper:]')

while [ $# -gt 0 ]; do
  case "$1" in
    --cooldown) COOLDOWN="${2:?}"; shift 2 ;;
    --reads)    READS="${2:?}";    shift 2 ;;
    --gb)       GB="${2:?}";       shift 2 ;;
    --no-erase) ERASE=0;           shift   ;;
    --replug)   REPLUG=1;          shift   ;;
    *) die "unknown option: $1" ;;
  esac
done

# Find f3write/f3read: PATH first, then a sibling build/ directory.
if command -v f3write >/dev/null; then
  F3WRITE=f3write; F3READ=f3read
elif [ -x "$(dirname "$0")/../build/f3write" ]; then
  F3WRITE="$(dirname "$0")/../build/f3write"; F3READ="$(dirname "$0")/../build/f3read"
else
  die "f3write not found. Install f3 (brew install f3) or build it first."
fi

# ---------------- safety ----------------
PLIST=$(diskutil info -plist "/dev/$DISK" 2>/dev/null) || die "no such disk: /dev/$DISK"
getval() { printf '%s' "$PLIST" | plutil -extract "$1" raw -o - - 2>/dev/null; }

INTERNAL=$(getval Internal); WHOLE=$(getval WholeDisk)
SIZE=$(getval Size);         PROTO=$(getval BusProtocol)
MEDIA=$(getval MediaName)

[ "$INTERNAL" = "false" ] || die "/dev/$DISK reports Internal=$INTERNAL — refusing."
[ "$WHOLE" = "true" ]     || die "/dev/$DISK is not a whole disk (pass disk8, not disk8s2)."
[ "${SIZE:-0}" -gt 0 ]    || die "could not read size of /dev/$DISK."

# ---- physical identity: USB location ID + serial for this BSD disk ----
# NB: `ioreg -l` prefixes lines with tree characters, so patterns are end-anchored only.
usb_via_ioreg() {
  ioreg -r -c IOUSBHostDevice -l -w0 2>/dev/null | awk -v want="$DISK" '
    /"locationID" = [0-9]+$/      { v=$0; sub(/.*= */,"",v); if (v+0 > 100000) loc=v }
    /"USB Serial Number" = ".*"$/ { s=$0; sub(/.*= */,"",s); gsub(/"/,"",s); ser=s }
    index($0, "\"BSD Name\" = \"" want "\"") {
      if (loc != "") printf "0x%x|%s\n", loc, substr(ser,1,24)
      else           printf "unknown|%s\n", substr(ser,1,24)
      exit }'
}
usb_via_profiler() {
  system_profiler SPUSBDataType 2>/dev/null | awk -v want="$DISK" '
    /Location ID:/   { loc=$0; sub(/^ *Location ID: */,"",loc); sub(/ .*/,"",loc) }
    /Serial Number:/ { ser=$0; sub(/^ *Serial Number: */,"",ser) }
    $0 ~ ("BSD Name: *" want "$") { print loc "|" substr(ser,1,24); exit }'
}
USB_RAW=$(usb_via_ioreg); [ -n "$USB_RAW" ] || USB_RAW=$(usb_via_profiler)
USB_LOC="${USB_RAW%%|*}"; USB_SER="${USB_RAW#*|}"
[ -n "$USB_LOC" ] || USB_LOC="unknown"
[ -n "$USB_SER" ] || USB_SER="unknown"

RANGE=""; [ -n "$GB" ] && RANGE="--end-at=$GB"   # scalar: bash 3.2 + set -u

cat <<EOF

  Drive label:   $NAME
  Volume name:   $VOLNAME
  Disk:          /dev/$DISK
  USB location:  $USB_LOC
  USB serial:    ${USB_SER:0:24}
  Media:         ${MEDIA:-unknown}   Bus: ${PROTO:-unknown}   Internal: $INTERNAL
  Size:          $SIZE bytes ($(echo "scale=2; $SIZE/1000000000" | bc) GB)
  Erase+write:   $([ $ERASE -eq 1 ] && echo yes || echo 'no (reusing existing .h2w files)')
  Scope:         ${GB:-full capacity}$([ -n "$GB" ] && echo ' GB')
  Cooldown:      ${COOLDOWN}s$([ $REPLUG -eq 1 ] && echo ' + physical replug')
  Reads:         $READS pass(es)

EOF
[ $ERASE -eq 1 ] && printf 'This ERASES /dev/%s. ' "$DISK"
printf 'Confirm this is drive "%s" — type GO: ' "$NAME"; read -r C
[ "$C" = "GO" ] || die "aborted."

LOGDIR="$HOME/f3-logs"; mkdir -p "$LOGDIR"
LOG="$LOGDIR/f3-${NAME}-$(date +%Y%m%d-%H%M%S).log"
exec > >(tee -a "$LOG") 2>&1

echo "=== f3 drive test (macOS) — $(date) ==="
echo "drive_label=$NAME volume=$VOLNAME disk=/dev/$DISK usb_location=$USB_LOC usb_serial=$USB_SER"
echo "size=$SIZE cooldown=${COOLDOWN}s replug=$REPLUG reads=$READS scope=${GB:-full}"
echo

mountpoint() {
  diskutil info -plist "/dev/${DISK}s2" 2>/dev/null \
    | plutil -extract MountPoint raw -o - - 2>/dev/null
}
remount() {
  local mp; mp=$(mountpoint)
  [ -n "$mp" ] && diskutil unmount "$mp" >/dev/null 2>&1
  diskutil mount "/dev/${DISK}s2" >/dev/null 2>&1
  sleep 2
}

# ---------------- erase + write ----------------
WSEC=0
if [ $ERASE -eq 1 ]; then
  info "Erasing as exFAT, volume name $VOLNAME ..."
  if ! diskutil eraseDisk ExFAT "$VOLNAME" "/dev/$DISK"; then
    info "Erase failed (Spotlight often holds the volume) — forcing unmount and retrying."
    diskutil unmountDisk force "/dev/$DISK" >/dev/null 2>&1; sleep 2
    diskutil eraseDisk ExFAT "$VOLNAME" "/dev/$DISK" || die "erase failed twice."
  fi
fi

MOUNT=$(mountpoint)
[ -n "${MOUNT:-}" ] && [ -d "$MOUNT" ] || die "could not find mount point for ${DISK}s2."
info "Volume mounted at $MOUNT"

if [ $ERASE -eq 1 ]; then
  info "Writing${GB:+ $GB GB} — the slow phase."
  W0=$(date +%s); "$F3WRITE" $RANGE "$MOUNT"; WRC=$?; W1=$(date +%s)
  WSEC=$((W1-W0))
  [ $WRC -eq 0 ] || { bad "WRITE FAILED after ${WSEC}s — see $LOG"; exit 1; }
  ok "Write completed in $((WSEC/60))m $((WSEC%60))s"
fi

# ---------------- cooldown ----------------
if [ $REPLUG -eq 1 ]; then
  info "Unmounting so you can physically unplug $NAME."
  diskutil unmountDisk "/dev/$DISK" >/dev/null 2>&1
  printf '  Unplug %s now, wait %ss, plug it back into the SAME port, then press Enter: ' "$NAME" "$COOLDOWN"
  read -r _
  info "Waiting for the volume to reappear..."
  for _ in $(seq 1 30); do MOUNT=$(mountpoint); [ -n "$MOUNT" ] && break; sleep 2; done
  [ -n "${MOUNT:-}" ] || die "volume did not reappear — re-check the identifier with 'diskutil list external physical'."
elif [ "$COOLDOWN" -gt 0 ]; then
  info "Cooling down ${COOLDOWN}s (unmounted, drive idle)."
  diskutil unmount "$MOUNT" >/dev/null 2>&1
  REM=$COOLDOWN
  while [ $REM -gt 0 ]; do
    printf '\r  %02d:%02d remaining   ' $((REM/60)) $((REM%60)); sleep 5; REM=$((REM-5))
  done
  printf '\r                        \r'
  remount; MOUNT=$(mountpoint); [ -n "${MOUNT:-}" ] || die "could not remount after cooldown."
else
  info "No cooldown — remounting to flush the page cache."
  remount; MOUNT=$(mountpoint); [ -n "${MOUNT:-}" ] || die "could not remount."
fi

# ---------------- reads ----------------
declare -a RESULTS
for i in $(seq 1 "$READS"); do
  echo; info "Read pass $i of $READS  [$NAME]"
  PASSLOG=$(mktemp); R0=$(date +%s)
  "$F3READ" $RANGE "$MOUNT" 2>&1 | tee "$PASSLOG"
  R1=$(date +%s); RSEC=$((R1-R0))

  if grep -q 'Data LOST' "$PASSLOG"; then
    LOST=$(grep -m1 'Data LOST' "$PASSLOG" | sed 's/.*Data LOST: *//')
    if printf '%s' "$LOST" | grep -q '^0\.00 Bytes'; then
      RESULTS+=("$i|COMPLETE|${RSEC}s|0 sectors lost")
      ok "  pass $i: complete, 0 sectors lost (${RSEC}s)"
    else
      RESULTS+=("$i|DATA LOST|${RSEC}s|$LOST")
      bad "  pass $i: DATA LOST — $LOST"
    fi
  else
    WHERE=$(grep -m1 'NOT fully read' "$PASSLOG" | sed 's/Validating file //;s/ \.\.\..*due to/ —/')
    RESULTS+=("$i|DROPOUT|${RSEC}s|${WHERE:-see log}")
    bad "  pass $i: DROPPED OFF after ${RSEC}s — ${WHERE:-see log}"
  fi
  rm -f "$PASSLOG"
  [ "$i" -lt "$READS" ] && { remount; MOUNT=$(mountpoint); }
done

# ---------------- summary ----------------
echo
echo "=============================================================="
printf ' SUMMARY — drive %s   volume %s\n' "$NAME" "$VOLNAME"
printf ' /dev/%s   USB location %s   serial %s\n' "$DISK" "$USB_LOC" "${USB_SER:0:16}"
echo "=============================================================="
[ $ERASE -eq 1 ] && printf ' write: %dm %02ds%s\n' $((WSEC/60)) $((WSEC%60)) "${GB:+  (first ${GB} GB)}"
printf ' cooldown: %ss%s\n' "$COOLDOWN" "$([ $REPLUG -eq 1 ] && echo ' + physical replug')"
echo ' ------------------------------------------------------------'
printf ' %-5s %-10s %-9s %s\n' PASS RESULT ELAPSED DETAIL
for r in ${RESULTS[@]+"${RESULTS[@]}"}; do
  IFS='|' read -r n res el det <<< "$r"
  printf ' %-5s %-10s %-9s %s\n' "$n" "$res" "$el" "$det"
done
echo '=============================================================='
echo "log: $LOG"
