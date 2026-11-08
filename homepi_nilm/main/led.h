// main/led.h
#pragma once
#include "driver/gpio.h"

#define LED_GPIO GPIO_NUM_4

typedef enum {
    LED_OFF,

    // ── Phases actives (série de blinks rapides + pause) ──
    LED_BOOT,            // ×3  blinks  80ms — démarrage
    LED_WIFI_CONNECTING, // ×2  blinks  80ms — connexion réseau
    LED_ZERO_TOUCH,      // ×4  blinks  60ms — enrollment X5C
    LED_OTA,             // ×5  blinks  60ms — mise à jour firmware

    // ── Attente action (blink lent) ───────────────────────
    LED_FALLBACK_WAIT,   // 800ms on/off — attente approbation opérateur

    // ── États stables ─────────────────────────────────────
    LED_OPERATIONAL,     // heartbeat : 60ms on / 3s off
    LED_ERROR,           // 200ms on/off continu
} led_state_t;

void led_init(void);
void led_set(led_state_t state);
void led_task_start(void);