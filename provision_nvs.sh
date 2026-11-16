#!/bin/bash
set -e

PORT=${1:-/dev/ttyACM0}
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SENSOR_DIR="$SCRIPT_DIR/homepi_sensor"

echo "=== HomePi NVS Provisioning ==="
echo "Port: $PORT"

# 1. Read chip_id from ESP32
echo "Reading chip info..."
CHIP_INFO=$(python -m esptool --chip esp32c3 -p "$PORT" chip_id 2>&1)
MAC=$(echo "$CHIP_INFO" | grep "MAC:" | head -1 | awk '{print $2}' | tr '[:upper:]' '[:lower:]' | tr -d '\r\n')
[ -z "$MAC" ] && echo "ERROR: Cannot read chip MAC" && exit 1

# Extract chip_id from MAC (last 4 bytes without first 2)
# MAC: 58:8c:81:9f:f7:b8 -> chip_id: 819ff7b8
CHIP_ID=$(echo "$MAC" | tr -d ':' | cut -c5- | tr -d '\r\n')
DEVICE_ID="esp32-${CHIP_ID}"

echo "MAC       : $MAC"
echo "chip_id   : $CHIP_ID"
echo "device_id : $DEVICE_ID"

# 2. Read fw_version from CMakeLists.txt
FW_VERSION=$(cd "$SENSOR_DIR" && git describe --always --tags --dirty 2>/dev/null || echo "unknown")
echo "fw_version: $FW_VERSION"

HW_MODEL="esp32c3"

# 3. WiFi parameters - from local file not versioned
WIFI_CONFIG="$SCRIPT_DIR/wifi_secrets.env"
[ ! -f "$WIFI_CONFIG" ] && echo "ERROR: $WIFI_CONFIG not found" && exit 1
WIFI_SSID=$(grep "^WIFI_SSID=" "$WIFI_CONFIG" | cut -d= -f2- | tr -d '\r\n')
WIFI_PASS=$(grep "^WIFI_PASS=" "$WIFI_CONFIG" | cut -d= -f2- | tr -d '\r\n')
echo "WIFI SSID : $WIFI_SSID"

# 4. Generate CSV - NO space around comas
# The CSV and the binary hold the Wi-Fi password: private temporary directory, removed on exit
WORK=$(mktemp -d)
chmod 700 "$WORK"
trap 'rm -rf "$WORK"' EXIT
NVS_CSV="$WORK/nvs_generated.csv"
{
  printf "key,type,encoding,value\n"
  printf "config,namespace,,\n"
  printf "device_id,data,string,%s\n" "$DEVICE_ID"
  printf "chip_id,data,string,%s\n"   "$CHIP_ID"
  printf "hw_model,data,string,%s\n"  "$HW_MODEL"
  printf "wifi_ssid,data,string,%s\n" "$WIFI_SSID"
  printf "wifi_pass,data,string,%s\n" "$WIFI_PASS"
  printf "enrolled,data,u8,0\n"
  printf "ldevid_slot,data,u8,0\n"
  printf "fw_version,data,string,%s\n" "$FW_VERSION"
} > "$NVS_CSV"

echo ""
echo "Generated NVS CSV (Wi-Fi password masked):"
sed -E 's/^(wifi_pass,data,string,).*/\1[masked]/' "$NVS_CSV"
echo ""

# 5. Generate NVS binary
NVS_BIN="$WORK/nvs_generated.bin"
python "$IDF_PATH/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py" generate "$NVS_CSV" "$NVS_BIN" 0x6000

echo "NVS binary generated (temporary, removed on exit)"

# 6. Flash NVS partition (offset 0x9000)
echo "Flashing NVS partition..."
python -m esptool --chip esp32c3 -p "$PORT" -b 460800 write_flash 0x9000 "$NVS_BIN"

echo ""
echo "=== Provisioning complete ==="
echo "device_id  : $DEVICE_ID"
echo "WiFi SSID  : $WIFI_SSID"
echo "fw_version : $FW_VERSION"
echo "Next step  : idf.py -p $PORT flash monitor"
