// main/nilm_bridge.h
//
// Pont UART -> MQTT pour la carte NILM (Teensy 4.0 -> XIAO ESP32-S3).
// Réutilise l'infrastructure d'enrollment/mTLS/MQTT existante telle quelle
// (enrollment.c, mqtt_app.c, nvs_manager.c) — ce module ne fait QUE :
//   1. Lire en continu les trames binaires envoyées par le Teensy sur UART
//      (protocole défini côté Teensy dans send_frame(), teensy_nilm_phase2_*.ino)
//   2. Les parser et les garder en cache (dernière valeur connue)
//   3. Exposer une fonction de publication MQTT suivant exactement la même
//      convention que sensors_publish() (JSON plat, topic iot/%s/sensors)
//
// Ce device se provisionne comme un device à part entière (son propre
// device_id, son propre enrollment zero-touch) via provision_device.sh /
// flash_device.sh existants — aucune modification de ces scripts requise.
#pragma once

#include "esp_err.h"
#include "mqtt_client.h"
#include <stdint.h>
#include <stdbool.h>

// ── Format de trame Teensy (voir send_frame() dans le firmware NILM) ──
// Header : 0xAA 0x55 0x01 <len>
// Payload (62 octets, little-endian, tel qu'écrit par le Teensy) :
//   float    V_rms
//   float    I1_rms
//   float    I2_rms
//   float    P_L1
//   float    P_L2
//   float    PF_L1
//   float    THD
//   float    H3
//   float    H5
//   float    H7
//   float    H9
//   int8_t   event_flag
//   float    delta_P
//   uint32_t ts              (millis() côté Teensy — horloge locale, PAS wall-clock)
//   float    lag_confirmed   (v2.2 — valeur de lag réellement utilisée pour
//                             les calculs de features, validée par consensus)
//   float    last_prominence (v2.2 — netteté du dernier pic de corrélation
//                             tenté, accepté ou non — diagnostic)
//   uint8_t  consensus_count (v2.2 — 0-3, remplissage de la fenêtre de
//                             consensus au moment de l'envoi)
// Suivi d'un CRC8 (poly 0x31, init 0xFF — identique à la fonction crc8()
// du firmware Teensy, à ne jamais faire diverger des deux côtés).
// IMPORTANT : ce format DOIT rester synchronisé avec send_frame() côté
// Teensy — toute modification de l'un sans l'autre invalide silencieusement
// toutes les trames (rejetées comme "longueur inattendue").
#define NILM_FRAME_HEADER_LEN   4
#define NILM_FRAME_PAYLOAD_LEN  62
#define NILM_FRAME_CRC_LEN      1
#define NILM_FRAME_TOTAL_LEN    (NILM_FRAME_HEADER_LEN + NILM_FRAME_PAYLOAD_LEN + NILM_FRAME_CRC_LEN)

typedef struct {
    float    V_rms, I1_rms, I2_rms;
    float    P_L1, P_L2, PF_L1;
    float    THD, H3, H5, H7, H9;
    int8_t   event_flag;
    float    delta_P;
    uint32_t teensy_ts_ms;   // horloge locale Teensy, pas wall-clock ESP32
    float    lag_confirmed;    // v2.2
    float    last_prominence;  // v2.2
    uint8_t  consensus_count;  // v2.2 — 0-3
    bool     valid;          // false tant qu'aucune trame valide n'a été reçue
    uint32_t frames_ok;      // compteur diagnostic
    uint32_t frames_crc_err; // compteur diagnostic
} nilm_reading_t;

// Initialise l'UART (RX depuis Teensy Serial1 TX) et démarre la tâche de
// lecture/parsing en tâche de fond. À appeler une fois au boot, comme
// sensors_init() — indépendant de l'état WiFi/enrollment/MQTT.
esp_err_t nilm_bridge_init(void);

// Recopie la dernière lecture valide dans *out. Retourne ESP_ERR_INVALID_STATE
// si aucune trame n'a encore été reçue depuis le boot.
esp_err_t nilm_bridge_get_latest(nilm_reading_t *out);

// Publie la dernière lecture sur le topic sensors MQTT du device, avec
// exactement la même convention JSON plate que sensors_publish() (voir
// sensors.c). À appeler depuis la boucle principale, comme
// mqtt_app_publish_sensors().
esp_err_t nilm_bridge_publish(esp_mqtt_client_handle_t client,
                               const char *device_id,
                               const char *topic_sensors);

// true si un événement de charge (event_flag != 0) est présent dans la
// dernière trame reçue — permet à la boucle principale de déclencher une
// publication immédiate plutôt que d'attendre le prochain tick périodique
// (un événement de charge ne doit pas attendre jusqu'à 30s pour apparaître
// sur le dashboard).
bool nilm_bridge_has_pending_event(void);