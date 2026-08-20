#!/bin/bash
set -e
VERSION=$1
FILTER=${2:-all}  # "all", device_id specifique, ou "fw:2.0.0" pour filtrer par version
PI=srvpi@192.168.0.151

[ -z "$VERSION" ] && echo "Usage: $0 <version> [device_id|all|fw:X.X.X]" && exit 1

echo "=== Build v${VERSION} ==="
cd ~/homepi-firmware/homepi_sensor

# Update version in CMakeLists.txt
sed -i "s/project(homepi_sensor VERSION .*/project(homepi_sensor VERSION ${VERSION})/" CMakeLists.txt
idf.py build

echo "=== Deploy to Pi ==="
scp build/homepi_sensor.bin $PI:/tmp/homepi_sensor.bin
ssh $PI "
  sudo cp /tmp/homepi_sensor.bin /opt/homepi/firmware/v${VERSION}.bin
  sudo sha256sum /opt/homepi/firmware/v${VERSION}.bin | cut -d' ' -f1 | \
  sudo tee /opt/homepifirmware/v${VERSION}.bin.sha256
"

SHA=$(ssh $PI "cat /opt/homepi/firmware/v${VERSION}.bin.sha256")
echo "SHA256: $SHA"

echo "=== Trigger OTA ==="
if [ "$FILTER" = "all" ]; then
  # All active devices
  DEVICES=$(ssh $PI "sqlite3 /var/lib/homepi/devices.db \
    \"SELECT device_id FROM devices WHERE status='active'\"")
elif [[ "$FILTER" == fw:* ]]; then
  # Filter by actual firmware version
  FW_FILTER=${FILTER#fw:}
  DEVICES=$(ssh $PI "sqlite3 /var/lib/homepi/devices.db \
    \"SELECT device_id FROM devices WHERE status='active' AND fw_version='${FW_FILTER}'\"")
else
  # Specific device
  DEVICES=$FILTER
fi

for DEVICE in $DEVICES; do
  echo " -> OTA for $DEVICE"
  ssh $PI "curl -s -X POST http://127.0.0.1:8091/api/admin/devices/${DEVICE}/ota \
    -H 'Content-Type: application/json' \
    -d '{\"version\":\"${VERSION}\",\"url\":\"https://homepi.internal/fw/v${VERSION}.bin\",\"checksum\":\"${SHA}\"}'"
done

echo "=== Done - monitoring: ==="
echo "mosquitto_sub -h 127.0.0.1 -p 1883 -u telegraf-bridge -t 'iot/+/sensors' -v"
