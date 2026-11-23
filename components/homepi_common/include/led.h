// main/led.h
#pragma once
#include "driver/gpio.h"

#define LED_GPIO GPIO_NUM_4

typedef enum {
    LED_OFF,

    // ── Active phases (series of quick blinks + pause) ──
    LED_BOOT,            // ×3  blinks  80ms: boot
    LED_WIFI_CONNECTING, // ×2  blinks  80ms: network connection
    LED_ZERO_TOUCH,      // ×4  blinks  60ms — enrollment X5C
    LED_OTA,             // ×5  blinks  60ms: firmware update

    // ── Waiting for an action (slow blink) ───────────────────────
    LED_FALLBACK_WAIT,   // 800ms on/off: waiting for operator approval

    // ── Steady states ─────────────────────────────────────
    LED_OPERATIONAL,     // heartbeat : 60ms on / 3s off
    LED_ERROR,           // 200ms on/off, continuous
} led_state_t;

void led_init(void);
void led_set(led_state_t state);
void led_task_start(void);