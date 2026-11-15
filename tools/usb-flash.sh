#!/usr/bin/env bash
# tools/usb-flash.sh: flash the HomePi device plugged in over USB with the project that
# fleet/devices.csv assigns to it, built from a signed tag, then check on the Pi that no new
# certificate was issued.
#
#   tools/usb-flash.sh v2.4.0
#
# Flashes bootloader, partition table, otadata and app; the NVS (identity, Wi-Fi) is never written.
# The USB flash is needed once per device (the bootloader carries the rollback setting); later
# updates go over the air.
set -uo pipefail

TAG=${1:-}
[ -n "$TAG" ] || { echo "usage: tools/usb-flash.sh <signed tag>"; exit 2; }
PI=${HOMEPI_SSH:-homepi}
REPO=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "STOPPED: run from the homepi-firmware repository"; exit 1; }
MANIFEST="$REPO/fleet/devices.csv"
LOG=${TMPDIR:-/tmp}/homepi-usb-flash
mkdir -p "$LOG"

stop() { echo "STOPPED: $*"; exit 1; }

echo "=== 1. signed tag $TAG"
git -C "$REPO" fetch -q --tags github 2>/dev/null || true
git -C "$REPO" tag -v "$TAG" > /dev/null 2>&1 || stop "$TAG is missing or its signature does not verify"
echo "  $TAG verified on $(git -C "$REPO" rev-parse --short "$TAG^{commit}")"

echo "=== 2. device on USB"
# Certificates issued from here on (and in the 15 minutes before: the previous firmware may have
# re-enrolled while the device was being plugged in) count against the verdict
T0=$(( $(date +%s) - 900 ))
PORT=""
for c in /dev/ttyACM* /dev/ttyUSB*; do [ -e "$c" ] && PORT=$c && break; done
[ -n "$PORT" ] || stop "no serial port (plug the device in, then usbipd attach)"
if [ -z "${IDF_PATH:-}" ]; then
  for d in ~/esp/esp-idf ~/esp-idf; do [ -f "$d/export.sh" ] && IDF_PATH=$d && break; done
fi
[ -n "${IDF_PATH:-}" ] || stop "ESP-IDF not found"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" > /dev/null 2>&1 || stop "export.sh failed"
INFO=$(esptool.py --port "$PORT" chip_id 2>&1)
CHIP=$(echo "$INFO" | grep -oE 'Chip is ESP32-[A-Z0-9]+' | head -1 | awk '{print $3}')
MAC=$(echo "$INFO" | grep "MAC:" | tail -1 | awk '{print $2}')
[ -n "$CHIP" ] && [ -n "$MAC" ] || stop "esptool could not read the chip on $PORT"
DEV="esp32-$(echo "$MAC" | tr -d ':' | tr '[:upper:]' '[:lower:]' | cut -c5-12)"
echo "  $PORT: $DEV ($CHIP)"

LINE=$(grep -v '^#' "$MANIFEST" | grep "^$DEV,") || stop "$DEV is not in fleet/devices.csv: add it before flashing"
PROJECT=$(echo "$LINE" | cut -d, -f2); WANT_CHIP=$(echo "$LINE" | cut -d, -f3)
echo "  manifest: $PROJECT on $WANT_CHIP ($(echo "$LINE" | cut -d, -f4-))"
[ "$CHIP" = "$WANT_CHIP" ] || stop "the chip is $CHIP but the manifest says $WANT_CHIP: fix the manifest or check the device"
[ -d "$REPO/$PROJECT" ] || stop "project $PROJECT does not exist"

ROW=$(ssh "$PI" "sudo sqlite3 -readonly 'file:/var/lib/homepi/devices.db?mode=ro' \"SELECT status || '|' || ifnull(fw_version,'-') || '|' || ifnull(hw_model,'-') || '|' || substr(ifnull(cert_fingerprint,'-'),1,16) FROM devices WHERE device_id='$DEV';\"") \
  || stop "ssh $PI failed"
[ -n "$ROW" ] || stop "$DEV is not in the registry"
echo "  registry before: $ROW"
case "$ROW" in active*) ;; *) stop "$DEV is ${ROW%%|*}, not active";; esac
case "$ROW" in *"|$TAG|"*) stop "$DEV already runs $TAG";; esac

echo "=== 3. build $PROJECT at $TAG (clean worktree, committed sdkconfig)"
WT=$(mktemp -d)
cleanup() { git -C "$REPO" worktree remove --force "$WT" > /dev/null 2>&1; rm -rf "$WT"; }
trap cleanup EXIT
git -C "$REPO" worktree add -q --detach "$WT" "$TAG" || stop "cannot create a worktree for $TAG"
B="$WT/$PROJECT/build"
idf.py -C "$WT/$PROJECT" -B "$B" build > "$LOG/build.log" 2>&1 \
  || { grep -E "error:" "$LOG/build.log" | head -10; stop "build failed (full log: $LOG/build.log)"; }
VER=$(grep -oE "App \"$PROJECT\" version: [^ ]+" "$LOG/build.log" | awk '{print $NF}')
TARGET=$(grep -oE 'CONFIG_IDF_TARGET "[a-z0-9]+"' "$B/config/sdkconfig.h" | cut -d'"' -f2)
WANT_TARGET=$(echo "$WANT_CHIP" | tr -d '-' | tr '[:upper:]' '[:lower:]')
[ "$VER" = "$TAG" ] || stop "built version is $VER, not $TAG"
[ "$TARGET" = "$WANT_TARGET" ] || stop "built for $TARGET, the device is $WANT_TARGET"
grep -q 'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1' "$B/config/sdkconfig.h" || stop "rollback not compiled in"
echo "  $PROJECT $VER for $TARGET, rollback compiled in"

echo "=== 4. flash (NVS untouched) and boot log 120 s"
idf.py -C "$WT/$PROJECT" -B "$B" -p "$PORT" flash > "$LOG/flash.log" 2>&1 \
  || { tail -5 "$LOG/flash.log"; stop "flash failed (full log: $LOG/flash.log)"; }
echo "  verified regions: $(grep -c 'Hash of data verified' "$LOG/flash.log")"
python3 - "$PORT" > "$LOG/boot.log" 2>&1 << 'PY'
import sys, time, serial
s = serial.Serial(sys.argv[1], 115200, timeout=1)
s.dtr = False; s.rts = True; time.sleep(0.1); s.rts = False   # reset to catch the boot
end = time.time() + 120
while time.time() < end:
    line = s.readline()
    if line:
        print(line.decode(errors="replace").rstrip())
PY
grep -a -E " boot$|hw_model|LDevID|HTTPS|Registered|Registration|Enroll|Connected to broker|E \(" "$LOG/boot.log" \
  | sed -E 's/\x1b\[[0-9;]*m//g' | tr -cd '\11\12\40-\176' | sed -E 's/[A-Za-z0-9_+\/=-]{60,}/<masked>/g' \
  | cut -c1-150 | head -12 | sed 's/^/  /'

echo "=== 5. Pi after the flash"
ssh "$PI" "sudo bash -s $DEV $T0" << 'PI_EOF'
DEV=$1; T0=$2
Q() { sqlite3 -readonly "file:/var/lib/homepi/devices.db?mode=ro" "$1"; }
echo "  registry after: $(Q "SELECT status || '|' || fw_version || '|' || ifnull(hw_model,'-') || '|' || substr(ifnull(cert_fingerprint,'-'),1,16) || '|exp=' || ifnull(cert_expiry,'-') FROM devices WHERE device_id='$DEV';")"
J=$(journalctl -u step-ca --since @"$T0" --no-pager -o cat)
S=$(echo "$J" | grep -c path=/1.0/sign); R=$(echo "$J" | grep -c path=/1.0/renew)
echo "  step-ca since 15 min before the device was plugged in: sign $S, renew $R"
Q "SELECT '  audit: ' || ts || ' ' || event || ' ' || substr(detail,1,90) FROM audit_log WHERE device_id='$DEV' AND ts >= strftime('%Y-%m-%dT%H:%M:%SZ',$T0,'unixepoch') ORDER BY id;"
# The LDevID lives 365 days: its issue time is the expiry minus 365 days. Issued inside the window
# (15 minutes before the device was plugged in, or later) means a re-enrollment around this flash;
# issued earlier the same day is only reported (the previous firmware re-enrolled at some outage).
EXP=$(Q "SELECT cert_expiry FROM devices WHERE device_id='$DEV';")
ISSUED=$(( $(date -d "$EXP" +%s) - 365 * 86400 ))
AGE_H=$(( ($(date +%s) - ISSUED) / 3600 ))
[ "$AGE_H" -lt 24 ] && echo "  note: the certificate in use was issued ${AGE_H} h ago ($(date -d @"$ISSUED" '+%F %T')), by the previous firmware"
if [ "$S" = 0 ] && [ "$R" = 0 ] && [ "$ISSUED" -lt "$T0" ]; then
  echo "  VERDICT: OK, no new certificate (a fingerprint change only corrects a stale v2.3.x registry row)"
else
  echo "  VERDICT: STOP, a certificate was issued around this flash: find out by which firmware before the next device"; exit 1
fi
PI_EOF
