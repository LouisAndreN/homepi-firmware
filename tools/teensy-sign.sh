#!/usr/bin/env bash
# tools/teensy-sign.sh: sign a Teensy 4.0 program for the remote update through the NILM bridge.
#
#   tools/teensy-sign.sh <program.hex> <version vMAJOR.MINOR.0>
#
# <program.hex>: Arduino IDE, Sketch > Export Compiled Binary (teensy_nilm_phase2_vX_Y.ino.hex).
# Output in ~/homepi-ota/<version>/homepi_nilm_teensy/: image.bin, manifest.json, manifest.sig, the
# same files and the same OTA key as tools/ota-sign.sh, so the Pi's upload and the bridge's checks
# are those of the ESP32 images.
#
# Refuses an image that:
#  - does not start at the Teensy 4.0 flash base (0x60000000) or does not fit the remote update;
#  - lacks "fw_teensy40" (not a Teensy 4.0 program), the remote-update marker (the next update would
#    need USB), or the version text HOMEPI_TEENSY_FW=<version> (labelled with another version).
set -uo pipefail

HEX=${1:-}; VER=${2:-}
[ -n "$HEX" ] && [ -n "$VER" ] || { echo "usage: tools/teensy-sign.sh <program.hex> <vMAJOR.MINOR.0>"; exit 2; }
REPO=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "STOPPED: run from the homepi-firmware repository"; exit 1; }
KEY=${HOMEPI_OTA_KEY:-$HOME/homepi-pki/ota-signing/ota-signing-key.pem}
PUB="$REPO/components/homepi_ota/ota_signing_pub.pem"
PROJECT=homepi_nilm_teensy; TARGET=teensy40
stop() { echo "STOPPED: $*"; exit 1; }

[[ "$VER" =~ ^v[0-9]+\.[0-9]+\.0$ ]] || stop "version must be vMAJOR.MINOR.0 (the Teensy reports major.minor)"
[ -f "$HEX" ] || stop "no such file: $HEX"
[ -f "$KEY" ] || stop "signing key not found: $KEY"
[ -f "$PUB" ] || stop "public key not found: $PUB"

OUT="$HOME/homepi-ota/$VER/$PROJECT"
mkdir -p "$OUT" && chmod 0700 "$HOME/homepi-ota"

echo "=== 1. $HEX -> binary image"
python3 -I - "$HEX" "$OUT/image.bin" "$VER" <<'PY' || stop "image refused"
import sys
hexpath, out, ver = sys.argv[1], sys.argv[2], sys.argv[3]
BASE, MAX = 0x60000000, 1024 * 1024
mem, upper = {}, 0
for n, line in enumerate(open(hexpath), 1):
    line = line.strip()
    if not line:
        continue
    if line[0] != ':':
        sys.exit(f"line {n}: not Intel HEX")
    b = bytes.fromhex(line[1:])
    if sum(b) & 0xFF:
        sys.exit(f"line {n}: checksum error")
    cnt, addr, typ, data = b[0], (b[1] << 8) | b[2], b[3], b[4:4 + b[0]]
    if typ == 0:
        a = upper + addr
        for i, v in enumerate(data):
            mem[a + i] = v
    elif typ == 1:
        break
    elif typ == 4:
        upper = ((data[0] << 8) | data[1]) << 16
    elif typ == 2:
        upper = ((data[0] << 8) | data[1]) << 4
    elif typ in (3, 5):
        pass
    else:
        sys.exit(f"line {n}: record type {typ}")
if not mem:
    sys.exit("empty image")
lo, hi = min(mem), max(mem) + 1
if lo != BASE:
    sys.exit(f"image starts at 0x{lo:08X}, not the Teensy 4.0 flash base 0x{BASE:08X}")
size = (hi - lo + 3) & ~3
if size > MAX:
    sys.exit(f"{size} bytes: larger than the {MAX} bytes the remote update accepts")
img = bytearray(b"\xff" * size)
for a, v in mem.items():
    img[a - lo] = v
for needle, why in ((b"fw_teensy40", "not a Teensy 4.0 program (FLASH_ID missing)"),
                    (b"HOMEPI_FX_REMOTE_UPDATE_V1", "no remote-update code: the next update would need USB"),
                    (f"HOMEPI_TEENSY_FW={ver}".encode(), f"the image does not say {ver}")):
    if needle not in img:
        sys.exit(why)
open(out, "wb").write(img)
print(f"  {size} bytes from 0x{lo:08X}, target fw_teensy40, remote-update marker, HOMEPI_TEENSY_FW={ver}")
PY

SHA=$(sha256sum "$OUT/image.bin" | cut -d' ' -f1)
echo "  sha256 ${SHA:0:16}..."

echo "=== 2. manifest, signature (passphrase of the OTA key), verification with the bridge's public key"
python3 -I -c 'import json, sys; sys.stdout.write(json.dumps({"chip_target": sys.argv[1], "project": sys.argv[2], "sha256": sys.argv[3], "version": sys.argv[4]}, sort_keys=True, separators=(",", ":")))' \
  "$TARGET" "$PROJECT" "$SHA" "$VER" > "$OUT/manifest.json"
openssl dgst -sha256 -sign "$KEY" -out "$OUT/manifest.sig.der" "$OUT/manifest.json" || stop "signing failed"
openssl dgst -sha256 -verify "$PUB" -signature "$OUT/manifest.sig.der" "$OUT/manifest.json" > /dev/null \
  || { rm -f "$OUT/manifest.sig.der"; stop "the signature does not verify with the public key the bridge carries: wrong key"; }
base64 -w0 "$OUT/manifest.sig.der" > "$OUT/manifest.sig"; rm -f "$OUT/manifest.sig.der"
echo "  manifest: $(cat "$OUT/manifest.json")"
echo "  signature: $(wc -c < "$OUT/manifest.sig") base64 characters, verified"
echo "  staged in $OUT"
