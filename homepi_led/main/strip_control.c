// main/strip_control.c
#include "strip_control.h"
#include "config.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

static const char *TAG = "strip_ctl";
static led_strip_handle_t s_strip;

typedef enum { PAT_SOLID, PAT_WAVE, PAT_BLINK, PAT_CHASE, PAT_RAINBOW } pattern_t;

typedef struct {
    bool     enabled;
    pattern_t pattern;
    uint8_t  r, g, b;
    uint8_t  brightness;
    float    speed;
    float    amplitude;
    float    frequency;
    float    opacity;
    int8_t   direction;
    int      tail_length;
    float    smooth;
    bool     replace_below;
    float    phase;
} layer_t;

static layer_t s_layers[STRIP_MAX_LAYERS];
static int s_layer_count = 0;

// ── Mutex FreeRTOS — remplace l'ancienne portENTER_CRITICAL/portEXIT_CRITICAL.
// Reason for the change: a critical section disables interrupts and
// suspends the scheduler on the current core. cJSON_ArrayForEach() has an unbounded
// duration (it depends on the MQTT payload size): calling anything in it
// that tries to take a lock (ESP_LOGI -> vprintf -> newlib lock)
// causes an immediate abort() (lock_acquire_generic detects the inconsistency).
// A plain FreeRTOS mutex has no such constraint: it blocks the calling
// task without disabling global interrupts, so it is compatible with
// JSON parsing and logging inside the protected section.
static SemaphoreHandle_t s_layer_mutex = NULL;

static void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float c = v * s;
    float x = c * (1 - fabsf(fmodf(h / 60.0f, 2) - 1));
    float m = v - c;
    float rf, gf, bf;
    if (h < 60)       { rf = c; gf = x; bf = 0; }
    else if (h < 120) { rf = x; gf = c; bf = 0; }
    else if (h < 180) { rf = 0; gf = c; bf = x; }
    else if (h < 240) { rf = 0; gf = x; bf = c; }
    else if (h < 300) { rf = x; gf = 0; bf = c; }
    else              { rf = c; gf = 0; bf = x; }
    *r = (uint8_t)((rf + m) * 255);
    *g = (uint8_t)((gf + m) * 255);
    *b = (uint8_t)((bf + m) * 255);
}

static pattern_t parse_pattern(const char *s)
{
    if (!strcmp(s, "wave"))    return PAT_WAVE;
    if (!strcmp(s, "blink"))   return PAT_BLINK;
    if (!strcmp(s, "chase"))   return PAT_CHASE;
    if (!strcmp(s, "rainbow")) return PAT_RAINBOW;
    return PAT_SOLID;
}

void strip_control_apply_composition(cJSON *layers_json)
{
    if (!layers_json) {
        ESP_LOGW(TAG, "Composition received without a 'layers' field");
        return;
    }

    if (!s_layer_mutex) {
        ESP_LOGE(TAG, "Mutex non initialise — strip_control_init() jamais appele ?");
        return;
    }

    // Temporary parsing buffer: the new composition is built
    // HERE, outside the lock, and the mutex is taken only for the final copy
    // (a nearly instant memcpy). This minimises how long the MQTT task holds
    // the lock, so the risk of making strip_task wait
    // (50 fps rendering on core 1) is close to zero.
    static layer_t parse_buf[STRIP_MAX_LAYERS];
    int parsed_count = 0;

    cJSON *item;
    cJSON_ArrayForEach(item, layers_json) {
        if (parsed_count >= STRIP_MAX_LAYERS) break;
        layer_t *l = &parse_buf[parsed_count];

        cJSON *enabled = cJSON_GetObjectItem(item, "enabled");
        l->enabled = enabled ? cJSON_IsTrue(enabled) : true;

        cJSON *pattern = cJSON_GetObjectItem(item, "pattern");
        l->pattern = parse_pattern((pattern && cJSON_IsString(pattern)) ? pattern->valuestring : "solid");

        cJSON *color = cJSON_GetObjectItem(item, "color");
        const char *hex = (color && cJSON_IsString(color)) ? color->valuestring : "#FFFFFF";
        unsigned int rgb = 0xFFFFFF;
        sscanf(hex, "#%06x", &rgb);
        l->r = (rgb >> 16) & 0xFF;
        l->g = (rgb >> 8) & 0xFF;
        l->b = rgb & 0xFF;

        cJSON *br = cJSON_GetObjectItem(item, "brightness");
        l->brightness = br ? (uint8_t)br->valuedouble : 255;

        cJSON *sp = cJSON_GetObjectItem(item, "speed");
        l->speed = sp ? (float)sp->valuedouble : 1.0f;

        cJSON *amp = cJSON_GetObjectItem(item, "amplitude");
        l->amplitude = amp ? (float)amp->valuedouble : 1.0f;

        cJSON *freq = cJSON_GetObjectItem(item, "frequency");
        l->frequency = freq ? (float)freq->valuedouble : 1.0f;

        cJSON *op = cJSON_GetObjectItem(item, "opacity");
        l->opacity = op ? (float)op->valuedouble : 1.0f;

        cJSON *dir = cJSON_GetObjectItem(item, "direction");
        l->direction = dir ? (int8_t)dir->valuedouble : 1;

        cJSON *tail = cJSON_GetObjectItem(item, "tailLength");
        l->tail_length = tail ? (int)tail->valuedouble : 3;
        if (l->tail_length < 1) l->tail_length = 1;

        cJSON *smooth = cJSON_GetObjectItem(item, "smooth");
        l->smooth = smooth ? (float)smooth->valuedouble : 0.5f;

        cJSON *repl = cJSON_GetObjectItem(item, "replaceBelow");
        l->replace_below = repl ? cJSON_IsTrue(repl) : false;

        l->phase = 0;
        parsed_count++;
    }

    // Diagnostic log: OUTSIDE any protected section, on the local buffer.
    // Safe: parse_buf is 'static', so there is no risk of pointing into the stack.
    for (int i = 0; i < parsed_count; i++) {
        ESP_LOGI(TAG, "Layer %d: pattern=%d direction=%d speed=%.2f replace_below=%d",
                 i, parse_buf[i].pattern, parse_buf[i].direction,
                 parse_buf[i].speed, parse_buf[i].replace_below);
    }

    // Final copy under the mutex: fast, no logging or allocation here.
    if (xSemaphoreTake(s_layer_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        memcpy(s_layers, parse_buf, sizeof(layer_t) * parsed_count);
        s_layer_count = parsed_count;
        xSemaphoreGive(s_layer_mutex);
        ESP_LOGI(TAG, "Composition appliquee : %d layers", parsed_count);
    } else {
        ESP_LOGE(TAG, "Timeout acquisition mutex — composition ignoree");
    }
}

static void render_layer(layer_t *l, uint32_t *r_out, uint32_t *g_out, uint32_t *b_out, int i)
{
    float bscale = (l->brightness / 255.0f) * l->opacity;
    float dphase = l->phase * l->direction;
    float rf = 0, gf = 0, bf = 0;

    switch (l->pattern) {
        case PAT_SOLID:
            rf = l->r * bscale; gf = l->g * bscale; bf = l->b * bscale;
            break;
        case PAT_WAVE: {
            float x = (float)i / STRIP_NUM_LEDS * l->frequency + dphase;
            float w = (sinf(x * 2 * (float)M_PI) + 1.0f) / 2.0f;
            float inten = 1.0f - l->amplitude + l->amplitude * w;
            rf = l->r * bscale * inten; gf = l->g * bscale * inten; bf = l->b * bscale * inten;
            break;
        }
        case PAT_BLINK: {
            bool on = fmodf(dphase, 1.0f) < 0.5f;
            if (on) { rf = l->r * bscale; gf = l->g * bscale; bf = l->b * bscale; }
            break;
        }
        case PAT_CHASE: {
            int pos = ((int)(dphase * STRIP_NUM_LEDS)) % STRIP_NUM_LEDS;
            if (pos < 0) pos += STRIP_NUM_LEDS;
            int dist = i - pos;
            if (dist < 0) dist += STRIP_NUM_LEDS;
            if (dist < l->tail_length) {
                float fade = 1.0f - (dist / (float)l->tail_length);
                float sharpness = 1.0f + (1.0f - l->smooth) * 4.0f;
                fade = powf(fade, sharpness);
                rf = l->r * bscale * fade; gf = l->g * bscale * fade; bf = l->b * bscale * fade;
            }
            break;
        }
        case PAT_RAINBOW: {
            uint8_t rr, gg, bb;
            float hue = fmodf((float)i / STRIP_NUM_LEDS + dphase, 1.0f) * 360.0f;
            if (hue < 0) hue += 360.0f;
            hsv_to_rgb(hue, 1.0f, bscale, &rr, &gg, &bb);
            rf = rr; gf = gg; bf = bb;
            break;
        }
    }
    *r_out += (uint32_t)rf; *g_out += (uint32_t)gf; *b_out += (uint32_t)bf;
}

static void render_and_show(float dt)
{
    // Read protected by the mutex: short timeout because the 50 fps rendering
    // must never block waiting for an MQTT composition.
    // If the lock is not available (a composition is being applied),
    // render the previous frame rather than miss a whole cycle.
    if (xSemaphoreTake(s_layer_mutex, 0) == pdTRUE) {
        for (int i = 0; i < STRIP_NUM_LEDS; i++) {
            uint32_t r = 0, g = 0, b = 0;
            for (int li = 0; li < s_layer_count; li++) {
                if (!s_layers[li].enabled) continue;
                if (s_layers[li].replace_below) { r = 0; g = 0; b = 0; }
                render_layer(&s_layers[li], &r, &g, &b, i);
            }
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            led_strip_set_pixel(s_strip, i, g, r, b);   // swap R/G for GRB order
        }
        for (int li = 0; li < s_layer_count; li++) {
            if (s_layers[li].enabled && s_layers[li].pattern != PAT_SOLID) {
                s_layers[li].phase += s_layers[li].speed * 0.3f * dt;
            }
        }
        xSemaphoreGive(s_layer_mutex);
    }
    led_strip_refresh(s_strip);
}

static void strip_task(void *arg)
{
    int64_t last_us = esp_timer_get_time();
    while (1) {
        int64_t now_us = esp_timer_get_time();
        float dt = (now_us - last_us) / 1000000.0f;
        last_us = now_us;
        if (dt > 0.2f) dt = 0.02f;

        render_and_show(dt);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t strip_control_init(void)
{
    s_layer_mutex = xSemaphoreCreateMutex();
    if (!s_layer_mutex) {
        ESP_LOGE(TAG, "xSemaphoreCreateMutex failed");
        return ESP_ERR_NO_MEM;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = GPIO_LED_STRIP,
        .max_leds = STRIP_NUM_LEDS,
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_APB,  // RMT_CLK_SRC_APB instead of RMT_CLK_SRC_DEFAULT
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 128,   // larger RMT memory buffer
        .flags.with_dma = true,    // the ESP32-S3 supports RMT+GDMA: offloads the CPU/interrupts
    };
    esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed: %s", esp_err_to_name(ret));
        return ret;
    }
    led_strip_clear(s_strip);

    s_layers[0] = (layer_t){ true, PAT_SOLID, 255, 255, 255, 80, 0.0f, 0.0f, 0.0f, 1.0f, 1, 3, 0.5f, false, 0.0f };
    s_layer_count = 1;

    return ESP_OK;
}

void strip_control_task_start(void)
{
    xTaskCreatePinnedToCore(strip_task, "strip_task", 4096, NULL, 4, NULL, 1);
}