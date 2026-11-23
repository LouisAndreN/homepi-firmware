// components/homepi_common/src/led.c
#include "led.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>

static atomic_int s_state = LED_OFF;

void led_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(LED_GPIO, 0);
}

void led_set(led_state_t state) {
    atomic_store(&s_state, state);
}

// ── Helpers ──────────────────────────────────────────────────

static inline void led_on(int ms) {
    gpio_set_level(LED_GPIO, 1);
    vTaskDelay(ms / portTICK_PERIOD_MS);
}
static inline void led_off(int ms) {
    gpio_set_level(LED_GPIO, 0);
    vTaskDelay(ms / portTICK_PERIOD_MS);
}

// N quick blinks (on_ms on, off_ms off), then pause_ms
static void blinks(int n, int on_ms, int off_ms, int pause_ms) {
    for (int i = 0; i < n; i++) {
        led_on(on_ms);
        led_off(off_ms);
    }
    led_off(pause_ms);
}

// ── Main task ───────────────────────────────────────────

static void led_task(void *arg) {
    while (1) {
        switch ((led_state_t)atomic_load(&s_state)) {

        case LED_OFF:
            gpio_set_level(LED_GPIO, 0);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            break;

        case LED_BOOT:
            // ×3 blinks 80ms + 500ms pause
            blinks(3, 80, 80, 500);
            break;

        case LED_WIFI_CONNECTING:
            // ×2 blinks 80ms + 400ms pause
            blinks(2, 80, 80, 400);
            break;

        case LED_ZERO_TOUCH:
            // ×4 blinks 60ms + 600ms pause
            blinks(4, 60, 60, 600);
            break;

        case LED_OTA:
            // ×5 blinks 60ms + 300ms pause
            blinks(5, 60, 60, 300);
            break;

        case LED_FALLBACK_WAIT:
            // slow blink: "waiting for a human action"
            led_on(800);
            led_off(800);
            break;

        case LED_OPERATIONAL:
            // LED fixe = tout fonctionne
            gpio_set_level(LED_GPIO, 1);
            vTaskDelay(100 / portTICK_PERIOD_MS);
            break;

        case LED_ERROR:
            // blink rapide continu
            led_on(200);
            led_off(200);
            break;

        default:
            vTaskDelay(100 / portTICK_PERIOD_MS);
            break;
        }
    }
}

void led_task_start(void) {
    xTaskCreate(led_task, "led", 1536, NULL, 1, NULL);
}