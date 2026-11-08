#!/usr/bin/env python3
"""
Génère nvs_config.csv et nvs_certs.csv pour un device HomePi.
Usage : python3 generate_nvs.py <device_id_short> <wifi_pass> [hw_model] [scd41|bme688]
"""
import sys, csv, os
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes
import hmac as h, hashlib

# ── Args ──────────────────────────────────────────────────────
if len(sys.argv) < 3:
    print("Usage: python3 generate_nvs.py <chip_id_short> <wifi_pass> [hw_model]")
    print("  ex: python3 generate_nvs.py 819ff7b8 MyWifiPass esp32c3")
    sys.exit(1)

CHIP_ID_SHORT = sys.argv[1]
WIFI_PASS     = sys.argv[2]
HW_MODEL      = sys.argv[3] if len(sys.argv) > 3 else "esp32c3"
WIFI_SSID     = "Pi-IoT"
FW_VERSION    = "2.0.0"

DEVICE_ID = f"esp32-{CHIP_ID_SHORT}"
BASE      = f"devices/{DEVICE_ID}"

# ── Vérifier les fichiers requis ───────────────────────────────
required = [
    "master.key",
    f"{BASE}/idevid.crt",
    f"{BASE}/idevid.key",
]
for p in required:
    if not os.path.exists(p):
        print(f"ERROR: fichier manquant : {p}")
        sys.exit(1)

# Chercher root_ca
root_ca_candidates = [
    "step-ca/root_ca.crt",
    "root_ca.crt",
]
root_ca_path = next((p for p in root_ca_candidates if os.path.exists(p)), None)
if not root_ca_path:
    print("ERROR: root_ca.crt introuvable")
    sys.exit(1)

# ── HMAC key ──────────────────────────────────────────────────
MASTER_KEY = open("master.key", "rb").read()
hmac_key = HKDF(
    hashes.SHA256(), 32, None, b"homepi-device-v1"
).derive(MASTER_KEY + DEVICE_ID.encode())

pin = h.new(hmac_key, b"pin-v1", hashlib.sha256).hexdigest()[:6].upper()

# ── PEM helper ────────────────────────────────────────────────
def pem(path):
    return open(path).read().strip().replace('\n', '\\n')

# ── nvs_config.csv ────────────────────────────────────────────
config_rows = [
    ["key",         "type",      "encoding", "value"],
    ["config",      "namespace", "",         ""],
    ["device_id",   "data",      "string",   DEVICE_ID],
    ["chip_id",     "data",      "string",   CHIP_ID_SHORT],
    ["hw_model",    "data",      "string",   HW_MODEL],
    ["wifi_ssid",   "data",      "string",   WIFI_SSID],
    ["wifi_pass",   "data",      "string",   WIFI_PASS],
    ["enrolled",    "data",      "u8",       "0"],
    ["ldevid_slot", "data",      "u8",       "0"],
    ["fw_version",  "data",      "string",   FW_VERSION],
]

config_path = f"{BASE}/nvs_config.csv"
with open(config_path, "w", newline="") as f:
    csv.writer(f, quoting=csv.QUOTE_ALL).writerows(config_rows)

# ── nvs_certs.csv ─────────────────────────────────────────────
certs_rows = [
    ["key",          "type",      "encoding", "value"],
    ["certs",        "namespace", "",         ""],
    ["hmac_key",     "data",      "hex2bin",  hmac_key.hex()],
    ["idevid_cert",  "data",      "string",   pem(f"{BASE}/idevid.crt")],
    ["idevid_key",   "data",      "string",   pem(f"{BASE}/idevid.key")],
    ["root_ca",      "data",      "string",   pem(root_ca_path)],
    ["ldevid_slot",  "data",      "u8",       "0"],
]

certs_path = f"{BASE}/nvs_certs.csv"
with open(certs_path, "w", newline="") as f:
    csv.writer(f, quoting=csv.QUOTE_ALL).writerows(certs_rows)

# ── Résumé ────────────────────────────────────────────────────
print(f"""
✓ Généré pour {DEVICE_ID}
  {config_path}
  {certs_path}

  device_id  : {DEVICE_ID}
  hw_model   : {HW_MODEL}
  PIN fallback: {pin}
  root_ca    : {root_ca_path}
""")