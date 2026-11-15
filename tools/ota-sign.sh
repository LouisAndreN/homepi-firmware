#!/usr/bin/env bash
# tools/ota-sign.sh: build a project from a signed tag, sign its OTA manifest with the offline key,
# check the signature against the public key compiled into that image, and stage the result.
#
#   tools/ota-sign.sh <signed tag> <project> [--rollback-test]
#
# Output in ~/homepi-ota/<tag>/<project>/: image.bin, manifest.json, manifest.sig (base64 DER ECDSA).
# The key is read from ~/homepi-pki/ota-signing/ota-signing-key.pem (passphrase typed once, by hand).
# --rollback-test builds an image that never connects to the broker (CONFIG_HOMEPI_OTA_ROLLBACK_TEST):
# it never confirms itself and the bootloader must revert it. It is staged under <project>-rollback-test.
set -uo pipefail

TAG=${1:-}; PROJECT=${2:-}; MODE=${3:-}
[ -n "$TAG" ] && [ -n "$PROJECT" ] || { echo "usage: tools/ota-sign.sh <signed tag> <project> [--rollback-test]"; exit 2; }
case "$MODE" in ""|--rollback-test) ;; *) echo "unknown option $MODE"; exit 2;; esac
REPO=$(git rev-parse --show-toplevel 2>/dev/null) || { echo "STOPPED: run from the homepi-firmware repository"; exit 1; }
KEY=${HOMEPI_OTA_KEY:-$HOME/homepi-pki/ota-signing/ota-signing-key.pem}
LOG=${TMPDIR:-/tmp}/homepi-ota-sign; mkdir -p "$LOG"
stop() { echo "STOPPED: $*"; exit 1; }

echo "=== 1. signed tag $TAG, project $PROJECT${MODE:+ ($MODE)}"
[ -f "$KEY" ] || stop "signing key not found: $KEY"
git -C "$REPO" tag -v "$TAG" > /dev/null 2>&1 || stop "$TAG is missing or its signature does not verify"
WT=$(mktemp -d)
cleanup() { git -C "$REPO" worktree remove --force "$WT" > /dev/null 2>&1; rm -rf "$WT"; }
trap cleanup EXIT
git -C "$REPO" worktree add -q --detach "$WT" "$TAG" || stop "cannot create a worktree for $TAG"
[ -d "$WT/$PROJECT" ] || stop "project $PROJECT does not exist at $TAG"
PUB="$WT/components/homepi_ota/ota_signing_pub.pem"
[ -f "$PUB" ] || stop "$TAG has no OTA public key (signed OTA starts with v2.4.1)"
echo "  $TAG on $(git -C "$REPO" rev-parse --short "$TAG^{commit}"), public key in the image: $(openssl pkey -pubin -in "$PUB" -outform DER 2>/dev/null | sha256sum | cut -c1-16)..."

echo "=== 2. build"
if [ -z "${IDF_PATH:-}" ]; then
  for d in ~/esp/esp-idf ~/esp-idf; do [ -f "$d/export.sh" ] && IDF_PATH=$d && break; done
fi
[ -n "${IDF_PATH:-}" ] || stop "ESP-IDF not found"
# shellcheck disable=SC1091
. "$IDF_PATH/export.sh" > /dev/null 2>&1 || stop "export.sh failed"
B="$WT/$PROJECT/build"; EXTRA=()
if [ "$MODE" = --rollback-test ]; then
  # A copy of the committed sdkconfig plus the option: SDKCONFIG_DEFAULTS is ignored once a sdkconfig exists
  cp "$WT/$PROJECT/sdkconfig" "$WT/rollback-test.sdkconfig" && echo 'CONFIG_HOMEPI_OTA_ROLLBACK_TEST=y' >> "$WT/rollback-test.sdkconfig"
  EXTRA=(-D "SDKCONFIG=$WT/rollback-test.sdkconfig")
fi
idf.py -C "$WT/$PROJECT" -B "$B" "${EXTRA[@]}" build > "$LOG/build.log" 2>&1 \
  || { grep -E "error:" "$LOG/build.log" | head -10; stop "build failed (full log: $LOG/build.log)"; }
VER=$(grep -oE "App \"$PROJECT\" version: [^ ]+" "$LOG/build.log" | awk '{print $NF}')
TARGET=$(grep -oE 'CONFIG_IDF_TARGET "[a-z0-9]+"' "$B/config/sdkconfig.h" | cut -d'"' -f2)
[ "$VER" = "$TAG" ] || stop "built version is $VER, not $TAG"
grep -q 'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1' "$B/config/sdkconfig.h" || stop "rollback not compiled in"
RT=$(grep -c 'CONFIG_HOMEPI_OTA_ROLLBACK_TEST 1' "$B/config/sdkconfig.h")
[ "$MODE" = --rollback-test ] && [ "$RT" != 1 ] && stop "the rollback test option is not in the build"
[ "$MODE" != --rollback-test ] && [ "$RT" != 0 ] && stop "the rollback test option is in a normal build"
BIN="$B/$PROJECT.bin"
SHA=$(sha256sum "$BIN" | cut -d' ' -f1)
echo "  $PROJECT $VER for $TARGET, $(stat -c %s "$BIN") bytes, sha256 ${SHA:0:16}...${MODE:+, ROLLBACK TEST IMAGE}"

echo "=== 3. manifest, signature (passphrase of the OTA key), verification with the image's public key"
OUT="$HOME/homepi-ota/$TAG/$PROJECT${MODE:+-rollback-test}"
mkdir -p "$OUT" && chmod 0700 "$HOME/homepi-ota"
python3 -I -c 'import json, sys; sys.stdout.write(json.dumps({"chip_target": sys.argv[1], "project": sys.argv[2], "sha256": sys.argv[3], "version": sys.argv[4]}, sort_keys=True, separators=(",", ":")))' \
  "$TARGET" "$PROJECT" "$SHA" "$VER" > "$OUT/manifest.json"
openssl dgst -sha256 -sign "$KEY" -out "$OUT/manifest.sig.der" "$OUT/manifest.json" || stop "signing failed"
openssl dgst -sha256 -verify "$PUB" -signature "$OUT/manifest.sig.der" "$OUT/manifest.json" > /dev/null \
  || stop "the signature does not verify with the public key compiled into $TAG: wrong key"
base64 -w0 "$OUT/manifest.sig.der" > "$OUT/manifest.sig"; rm -f "$OUT/manifest.sig.der"
cp "$BIN" "$OUT/image.bin"
echo "  manifest: $(cat "$OUT/manifest.json")"
echo "  signature: $(wc -c < "$OUT/manifest.sig") base64 characters, verified"
echo "  staged in $OUT"
