// main/ota.h
#pragma once
#include "esp_err.h"

// Lance OTA depuis une URL HTTPS, vérifie SHA256 avant apply
// sha256_hex : string hex 64 caractères
esp_err_t ota_start(const char *url, const char *sha256_hex);

// Publie sur MQTT le détail du dernier échec OTA (raison + hash actual/expected
// si pertinent). À appeler UNIQUEMENT après que mqtt_handle soit reconnecté —
// ota_start() lui-même ne peut pas publier car MQTT est arrêté pendant l'OTA
// (libération mémoire TLS). Ne fait rien si aucun échec n'a été enregistré
// depuis le dernier appel (évite de republier un vieux résultat par erreur).
void ota_publish_last_failure(void);