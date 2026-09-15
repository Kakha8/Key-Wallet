// SPDX-License-Identifier: AGPL-3.0-only
// SSD1306 128x64, SDA GPIO11, SCL GPIO12. Called only by core0.
#include "wallet_ui.h"
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

static i2c_master_dev_handle_t oled;
static uint8_t frame[1024];
// Workers publish static labels; only core0 touches I2C and the framebuffer.
static _Atomic(const char *) command = "READY";
static const char *shown_command = "READY";
static const char *status = "READY";
static bool waiting;
static bool result_visible;
static TickType_t result_until;
static int previous_buttons = -1;
static TickType_t last_poll;
static const uint8_t letters[26][5] = {
 {0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},
 {0x3e,0x41,0x41,0x41,0x22},{0x7f,0x41,0x41,0x22,0x1c},
 {0x7f,0x49,0x49,0x49,0x41},{0x7f,0x09,0x09,0x09,0x01},
 {0x3e,0x41,0x49,0x49,0x7a},{0x7f,0x08,0x08,0x08,0x7f},
 {0,0x41,0x7f,0x41,0},{0x20,0x40,0x41,0x3f,0x01},
 {0x7f,0x08,0x14,0x22,0x41},{0x7f,0x40,0x40,0x40,0x40},
 {0x7f,0x02,0x0c,0x02,0x7f},{0x7f,0x04,0x08,0x10,0x7f},
 {0x3e,0x41,0x41,0x41,0x3e},{0x7f,0x09,0x09,0x09,0x06},
 {0x3e,0x41,0x51,0x21,0x5e},{0x7f,0x09,0x19,0x29,0x46},
 {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7f,0x01,0x01},
 {0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},
 {0x3f,0x40,0x38,0x40,0x3f},{0x63,0x14,0x08,0x14,0x63},
 {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43}
};
// Monochrome rendering of the backend favicon, scaled to 34x40 pixels.
static const uint64_t enigma_logo[40] = {
 0x000000000ULL,0x000000000ULL,0x000000000ULL,0x000000000ULL,
 0x07ffffff8ULL,0x0fffffffcULL,0x0fffffffcULL,0x0fffffff0ULL,
 0x0ffffffe0ULL,0x0e0001fc0ULL,0x0e0003f80ULL,0x0c0007f00ULL,
 0x0c000fe00ULL,0x00001fc00ULL,0x00003f800ULL,0x00007f000ULL,
 0x0000fe000ULL,0x0001fc000ULL,0x0003f8000ULL,0x0001f0000ULL,
 0x0000f8000ULL,0x0000fc000ULL,0x00007e000ULL,0x00003f000ULL,
 0x00001f000ULL,0x00000f800ULL,0x000007c00ULL,0x000007e00ULL,
 0x040003f00ULL,0x0e0001f80ULL,0x0e0000fc0ULL,0x0e00007e0ULL,
 0x0e00003e0ULL,0x0fffffff0ULL,0x0fffffff8ULL,0x0fffffffcULL,
 0x07ffffffcULL,0x03ffffff8ULL,0x000000000ULL,0x000000000ULL
};
static void pixel(unsigned x, unsigned y, bool on) {
    if (x >= 128 || y >= 64) return;
    uint8_t mask = (uint8_t)(1U << (y & 7));
    if (on) frame[(y >> 3) * 128 + x] |= mask;
    else frame[(y >> 3) * 128 + x] &= (uint8_t)~mask;
}
static void text(unsigned x, unsigned y, const char *s, bool on) {
    for (; *s && x + 5 < 128; ++s, x += 6) {
        if (*s < 'A' || *s > 'Z') continue;
        const uint8_t *glyph = letters[*s - 'A'];
        for (unsigned column = 0; column < 5; column++)
            for (unsigned row = 0; row < 7; row++)
                if (glyph[column] & (1U << row)) pixel(x + column, y + row, on);
    }
}
static void centered(unsigned y, const char *s, bool on) {
    size_t width = strlen(s) * 6;
    text(width < 128 ? (128 - width) / 2 : 0, y, s, on);
}
static void logo(unsigned x, unsigned y) {
    for (unsigned row = 0; row < 40; row++)
        for (unsigned column = 0; column < 34; column++)
            if (enigma_logo[row] & (1ULL << column))
                pixel(x + column, y + row, true);
}
static void fill_rect(unsigned x, unsigned y, unsigned w, unsigned h, bool on) {
    for (unsigned px = x; px < x + w; px++)
        for (unsigned py = y; py < y + h; py++) pixel(px, py, on);
}
static void outline(unsigned x, unsigned y, unsigned w, unsigned h) {
    fill_rect(x, y, w, 1, true); fill_rect(x, y + h - 1, w, 1, true);
    fill_rect(x, y, 1, h, true); fill_rect(x + w - 1, y, 1, h, true);
}
static void button(unsigned x, unsigned w, const char *label, bool pressed) {
    const unsigned y = 42, h = 20;
    if (pressed) fill_rect(x, y, w, h, true); else outline(x, y, w, h);
    size_t label_width = strlen(label) * 6;
    text(x + (w - label_width) / 2, y + 7, label, !pressed);
}
static bool flush(void) {
    if (!oled) return false;
    const uint8_t address[] = {0,0x21,0,127,0x22,0,7};
    if (i2c_master_transmit(oled,address,sizeof(address),50) != ESP_OK) return false;
    uint8_t packet[33] = {0x40};
    for (unsigned i=0;i<sizeof(frame);i+=32) {
        memcpy(packet+1,frame+i,32);
        if (i2c_master_transmit(oled,packet,sizeof(packet),50) != ESP_OK) return false;
    }
    return true;
}
static bool render(bool ok_pressed, bool cancel_pressed) {
    memset(frame, 0, sizeof(frame));
    if (waiting) {
        centered(3, shown_command, true);
        centered(22, "CONFIRM", true);
        button(4, 48, "OK", ok_pressed);
        button(58, 66, "CANCEL", cancel_pressed);
    } else if (strcmp(status, "READY") == 0) {
        logo(47, 0);
        centered(54, "ENIGMA WALLET", true);
    } else {
        centered(12, shown_command, true);
        centered(35, status, true);
    }
    return flush();
}
void wallet_ui_init(void) {
    gpio_config_t buttons = {.pin_bit_mask=(1ULL<<5)|(1ULL<<6),
        .mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_DISABLE,
        .pull_down_en=GPIO_PULLDOWN_ENABLE,.intr_type=GPIO_INTR_DISABLE};
    ESP_ERROR_CHECK(gpio_config(&buttons));
    i2c_master_bus_config_t config = {.i2c_port=I2C_NUM_0,
        .sda_io_num=GPIO_NUM_11,.scl_io_num=GPIO_NUM_12,
        .clk_source=I2C_CLK_SRC_DEFAULT,.glitch_ignore_cnt=7,
        .flags.enable_internal_pullup=true};
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&config,&bus) != ESP_OK) return;
    i2c_device_config_t dev = {.dev_addr_length=I2C_ADDR_BIT_LEN_7,
        .device_address=0x3c,.scl_speed_hz=400000};
    if (i2c_master_bus_add_device(bus,&dev,&oled) != ESP_OK) return;
    const uint8_t init[] = {0,0xae,0xd5,0x80,0xa8,0x3f,0xd3,0,0x40,
        0x8d,0x14,0x20,0,0xa1,0xc8,0xda,0x12,0x81,0x7f,
        0xd9,0xf1,0xdb,0x40,0xa4,0xa6,0xaf};
    if (i2c_master_transmit(oled,init,sizeof(init),50) != ESP_OK) {
        ESP_LOGE("wallet", "OLED initialization failed");
        oled = NULL;
        return;
    }
    render(false, false);
}
bool wallet_ui_prompt(void) {
    waiting = true;
    result_visible = false;
    shown_command = atomic_load(&command);
    status = "CONFIRM";
    previous_buttons = -1;
    return render(gpio_get_level(GPIO_NUM_5), gpio_get_level(GPIO_NUM_6));
}
void wallet_ui_result(const char *message) {
    waiting = false;
    result_visible = true;
    result_until = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    status = strstr(message, "CANCEL") ? "CANCELLED" : message;
    render(false, false);
    previous_buttons = -1;
}
void wallet_ui_command(const char *name) {
    atomic_store(&command, name);
}
void wallet_ui_task(void) {
    TickType_t now = xTaskGetTickCount();
    if ((TickType_t)(now - last_poll) < pdMS_TO_TICKS(100)) return;
    last_poll = now;
    int pins = (gpio_get_level(GPIO_NUM_5) ? 1 : 0) |
               (gpio_get_level(GPIO_NUM_6) ? 2 : 0);
    if (!waiting && result_visible &&
            (int32_t)(now - result_until) >= 0) {
        result_visible = false;
        shown_command = "READY";
        status = "READY";
        atomic_store(&command, "READY");
        render(false, false);
        previous_buttons = pins;
        return;
    }
    const char *next = atomic_load(&command);
    // Preserve the last operation/result through background GET INFO traffic.
    if (!waiting && !result_visible && next != shown_command &&
            strcmp(next, "GET INFO") != 0) {
        shown_command = next;
        status = "WORKING";
        previous_buttons = -1;
    }
    if (pins != previous_buttons) {
        render((pins & 1) != 0, (pins & 2) != 0);
        previous_buttons = pins;
    }
}
