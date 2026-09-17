// SPDX-License-Identifier: AGPL-3.0-only
// SSD1306 128x64, SDA GPIO11, SCL GPIO12. Called only by core0.
#include "wallet_ui.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_random.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "tusb.h"
#include "mbedtls/platform_util.h"
#include "crypt_blowfish.h"

static i2c_master_dev_handle_t oled;
static uint8_t frame[1024];
// Workers publish static labels; only core0 touches I2C and the framebuffer.
static _Atomic(const char *) command = "READY";
static _Atomic(TickType_t) removal_latched_until;
static const char *shown_command = "READY";
static const char *status = "READY";
static bool waiting;
static bool result_visible;
static bool settings_open;
static bool settings_detail_open;
static bool settings_change_confirm;
static bool settings_reset_confirm;
static bool reset_yes_armed;
static bool reset_success_visible;
static TickType_t reset_success_until;
static bool settings_change_success;
static TickType_t settings_change_success_until;
static unsigned settings_selection;
static unsigned device_info_page;
static uint32_t device_info_id;
static bool history_open;
static TickType_t result_until;
static int previous_buttons = -1;
static TickType_t last_poll;
typedef enum {
    PIN_IDLE, PIN_CREATE, PIN_RETYPE, PIN_VERIFY,
    PIN_CHANGE_CURRENT, PIN_CHANGE_NEW, PIN_CHANGE_RETYPE,
    PIN_RESET_CHALLENGE, PIN_RESET_VERIFY
} pin_mode_t;
static pin_mode_t pin_mode;
static int local_auth_result;
static char pin_entry[7], pin_first[7], pin_hash[61];
static unsigned pin_length, selected_key;
static const char pin_keys[] = "123456789 0 ";
static const char *pin_notice;
static TickType_t pin_back_started;
static bool pin_back_down;
static bool pin_back_cancelled;
static bool suppress_settings_back_release;
static nvs_handle_t pin_nvs;
static bool pin_store_open;
static int pin_previous_buttons = -1;
static bool pin_setup_at_boot;
static char reset_challenge[5];
static char reset_challenge_title[16];
void pin_bcrypt_yield(void) { vTaskDelay(1); }
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
static const uint8_t digits[10][5] = {
 {0x3e,0x51,0x49,0x45,0x3e},{0,0x42,0x7f,0x40,0},
 {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
 {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
 {0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
 {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e}
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
        const uint8_t *glyph;
        if (*s >= 'A' && *s <= 'Z') glyph = letters[*s - 'A'];
        else if (*s >= '0' && *s <= '9') glyph = digits[*s - '0'];
        else if (*s == ':') {
            pixel(x + 2, y + 2, on);
            pixel(x + 2, y + 5, on);
            continue;
        }
        else continue;
        for (unsigned column = 0; column < 5; column++)
            for (unsigned row = 0; row < 7; row++)
                if (glyph[column] & (1U << row)) pixel(x + column, y + row, on);
    }
}
static void centered(unsigned y, const char *s, bool on) {
    size_t width = strlen(s) * 6;
    text(width < 128 ? (128 - width) / 2 : 0, y, s, on);
}
static void tiny_text(unsigned x, unsigned y, const char *s, bool on) {
    for (; *s && x + 2 < 128; ++s, x += 4) {
        const uint8_t *glyph = NULL;
        static const uint8_t a[3]={0x1e,0x05,0x1e}, e[3]={0x1f,0x15,0x11},
            g[3]={0x0e,0x11,0x1d}, i[3]={0x11,0x1f,0x11},
            l[3]={0x1f,0x10,0x10}, m[3]={0x1f,0x02,0x1f},
            n[3]={0x1f,0x01,0x1e}, s_[3]={0x12,0x15,0x09},
            t[3]={0x01,0x1f,0x01}, w[3]={0x1f,0x08,0x1f},
            h[3]={0x1f,0x04,0x1f}, o[3]={0x0e,0x11,0x0e},
            r[3]={0x1f,0x05,0x1a}, yy[3]={0x03,0x1c,0x03};
        switch (*s) {
            case 'A': glyph=a; break; case 'E': glyph=e; break;
            case 'G': glyph=g; break; case 'I': glyph=i; break;
            case 'L': glyph=l; break; case 'M': glyph=m; break;
            case 'N': glyph=n; break; case 'S': glyph=s_; break;
            case 'T': glyph=t; break; case 'W': glyph=w; break;
            case 'H': glyph=h; break; case 'O': glyph=o; break;
            case 'R': glyph=r; break; case 'Y': glyph=yy; break;
            default: break;
        }
        if (!glyph) continue;
        for (unsigned column=0; column<3; ++column)
            for (unsigned row=0; row<5; ++row)
                if (glyph[column] & (1U<<row)) pixel(x+column,y+row,on);
    }
}
static void tiny_centered(unsigned y, const char *s) {
    size_t width = strlen(s) * 4;
    tiny_text(width < 128 ? (128 - width) / 2 : 0, y, s, true);
}
static void logo_small(unsigned x, unsigned y) {
    for (unsigned row = 0; row < 30; row++)
        for (unsigned column = 0; column < 25; column++)
            if (enigma_logo[(row * 40) / 30] & (1ULL << ((column * 34) / 25)))
                pixel(x + column, y + row, true);
}
static void gear(unsigned x, unsigned y, bool on) {
    for (int row = -6; row <= 6; ++row)
        for (int column = -6; column <= 6; ++column) {
            int ax = column < 0 ? -column : column;
            int ay = row < 0 ? -row : row;
            int distance = column * column + row * row;
            bool ring = distance >= 7 && distance <= 25;
            bool tooth = (ax <= 1 && ay >= 5) || (ay <= 1 && ax >= 5) ||
                         (ax >= 4 && ax <= 5 && ay >= 4 && ay <= 5);
            if (ring || tooth) pixel(x + column + 6, y + row + 6, on);
        }
}
static void history_icon(unsigned x, unsigned y, bool on) {
    for (int row = -5; row <= 5; ++row)
        for (int column = -5; column <= 5; ++column) {
            int distance = row * row + column * column;
            if (distance >= 16 && distance <= 29)
                pixel(x + column + 6, y + row + 6, on);
        }
    // Clock hands and the small return arrow distinguish this from a plain circle.
    for (unsigned i = 3; i <= 6; ++i) pixel(x + 6, y + i, on);
    for (unsigned i = 6; i <= 9; ++i) pixel(x + i, y + 6, on);
    pixel(x, y + 2, on); pixel(x, y + 3, on);
    pixel(x + 1, y + 2, on); pixel(x + 2, y + 2, on);
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
static void pin_button(unsigned x, unsigned w, const char *label, bool pressed) {
    const unsigned y = 50, h = 13;
    if (pressed) fill_rect(x, y, w, h, true); else outline(x, y, w, h);
    size_t label_width = strlen(label) * 6;
    text(x + (w - label_width) / 2, y + 3, label, !pressed);
}
static const char *pin_title(void) {
    if (pin_notice) return pin_notice;
    if (pin_mode == PIN_RESET_CHALLENGE) return reset_challenge_title;
    if (pin_mode == PIN_RESET_VERIFY) return "RESET PIN";
    if (pin_mode == PIN_CREATE) return "CREATE PIN";
    if (pin_mode == PIN_RETYPE) return "RETYPE PIN";
    if (pin_mode == PIN_CHANGE_CURRENT) return "CURRENT PIN";
    if (pin_mode == PIN_CHANGE_NEW) return "NEW PIN";
    if (pin_mode == PIN_CHANGE_RETYPE) return "RETYPE NEW PIN";
    if (strcmp(shown_command, "REGISTER") == 0) return "REGISTER PIN";
    if (strcmp(shown_command, "REMOVAL") == 0) return "REMOVE DEVICE";
    return "AUTH PIN";
}
static const char *settings_items[3] = {"DEVICE INFO", "CHANGE PIN", "RESET DEVICE"};
static void render_device_info(bool back_pressed, bool next_pressed) {
    char id_label[16];
    centered(1, "DEVICE INFO", true);
    if (device_info_page == 0) {
        text(5, 16, "NAME: ENIGMA WALLET", true);
        text(5, 28, "MODEL: ESP32 S3", true);
        if (device_info_id)
            snprintf(id_label, sizeof(id_label), "ID: %08lX", (unsigned long)device_info_id);
        else
            snprintf(id_label, sizeof(id_label), "ID: UNAVAILABLE");
        text(5, 40, id_label, true);
    } else {
        text(5, 18, pin_hash[0] ? "PIN: SET" : "PIN: NOT SET", true);
        text(5, 33, tud_mounted() ? "USB: CONNECTED" : "USB: DISCONNECTED", true);
    }
    pin_button(1, 48, "BACK", back_pressed);
    pin_button(79, 48, device_info_page == 0 ? "NEXT" : "PREV", next_pressed);
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
    if (reset_success_visible) {
        centered(20, "DEVICE SUCCESSFULLY", true);
        centered(34, "RESET", true);
    } else if (pin_mode != PIN_IDLE) {
        centered(1, pin_title(), true);
        unsigned entry_slots = pin_mode == PIN_RESET_CHALLENGE ? 4 : 6;
        for (unsigned i = 0; i < entry_slots; ++i) {
            outline(1 + i * 12, 29, 10, 18);
            if (i < pin_length) fill_rect(5 + i * 12, 37, 3, 3, true);
        }
        for (unsigned i = 0; i < 12; ++i) {
            if (pin_keys[i] == ' ') continue;
            unsigned x = 76 + (i % 3) * 17, y = 17 + (i / 3) * 12;
            bool selected = i == selected_key;
            if (selected) fill_rect(x, y, 15, 11, true); else outline(x, y, 15, 11);
            char digit[2] = {pin_keys[i], 0};
            text(x + 5, y + 2, digit, !selected);
        }
        pin_button(1, 41, pin_back_cancelled ? "CANCEL" : "BACK", ok_pressed);
        pin_button(47, 20, "OK", cancel_pressed);
    } else if (waiting) {
        centered(3, shown_command, true);
        centered(22, "CONFIRM", true);
        button(4, 48, "OK", ok_pressed);
        button(58, 66, "CANCEL", cancel_pressed);
    } else if (settings_open && strcmp(status, "READY") == 0) {
        if (settings_change_confirm) {
            centered(8, "CHANGE PIN", true);
            centered(25, "ARE YOU SURE", true);
            pin_button(1, 48, "BACK", ok_pressed);
            pin_button(79, 48, "YES", !gpio_get_level(GPIO_NUM_7));
        } else if (settings_reset_confirm) {
            centered(1, "RESET DEVICE", true);
            centered(14, "ALL KEYS WILL", true);
            centered(26, "BE DELETED", true);
            centered(38, "ARE YOU SURE", true);
            pin_button(1, 48, "BACK", ok_pressed);
            pin_button(79, 48, "YES", !gpio_get_level(GPIO_NUM_7));
        } else if (settings_change_success) {
            centered(16, "PIN SUCCESSFULLY", true);
            centered(31, "CHANGED", true);
            pin_button(1, 48, "BACK", ok_pressed);
        } else if (settings_detail_open) {
            if (settings_selection == 0)
                render_device_info(ok_pressed, !gpio_get_level(GPIO_NUM_7));
            else {
                centered(11, settings_items[settings_selection], true);
                centered(30, "COMING SOON", true);
                pin_button(1, 48, "BACK", ok_pressed);
            }
        } else {
            centered(1, "SETTINGS", true);
            for (unsigned i = 0; i < 3; ++i) {
                unsigned y = 13 + i * 12;
                bool selected = i == settings_selection;
                if (selected) fill_rect(4, y, 120, 10, true);
                else outline(4, y, 120, 10);
                text(11, y + 2, settings_items[i], !selected);
            }
            pin_button(1, 48, "BACK", ok_pressed);
            pin_button(79, 48, "OPEN", !gpio_get_level(GPIO_NUM_7));
        }
    } else if (history_open && strcmp(status, "READY") == 0) {
        centered(25, "COMING SOON", true);
        pin_button(38, 52, "BACK", cancel_pressed);
    } else if (strcmp(status, "READY") == 0) {
        logo_small(51, 3);
        tiny_centered(38, "ENIGMA WALLET");
        if (ok_pressed) fill_rect(1, 48, 65, 15, true);
        else outline(1, 48, 65, 15);
        gear(3, 49, !ok_pressed);
        tiny_text(20, 53, "SETTINGS", !ok_pressed);
        if (cancel_pressed) fill_rect(68, 48, 59, 15, true);
        else outline(68, 48, 59, 15);
        history_icon(70, 49, !cancel_pressed);
        tiny_text(86, 53, "HISTORY", !cancel_pressed);
    } else {
        centered(12, shown_command, true);
        centered(35, status, true);
    }
    return flush();
}
void wallet_ui_reset_on_boot(void) {
    if (nvs_flash_init() != ESP_OK) return;
    nvs_handle_t store;
    if (nvs_open("device-pin", NVS_READONLY, &store) != ESP_OK) return;
    uint8_t pending = 0;
    esp_err_t read_result = nvs_get_u8(store, "wipe-pending", &pending);
    nvs_close(store);
    if (read_result != ESP_OK || pending != 1) return;

    // This runs before USB, the flash filesystem, and its worker task start.
    const esp_partition_t *data = esp_partition_find_first(0x40, 0x1, "part0");
    ESP_ERROR_CHECK(data ? ESP_OK : ESP_ERR_NOT_FOUND);
    ESP_ERROR_CHECK(esp_partition_erase_range(data, 0, data->size));
    ESP_ERROR_CHECK(nvs_flash_erase());
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_open("device-pin", NVS_READWRITE, &store));
    ESP_ERROR_CHECK(nvs_set_u8(store, "reset-done", 1));
    ESP_ERROR_CHECK(nvs_commit(store));
    nvs_close(store);
    esp_restart();
}
void wallet_ui_init(void) {
    gpio_config_t buttons = {.pin_bit_mask=(1ULL<<5)|(1ULL<<6)|(1ULL<<7)|(1ULL<<10)|(1ULL<<17)|(1ULL<<18)|(1ULL<<21),
        .mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE,
        .pull_down_en=GPIO_PULLDOWN_DISABLE,.intr_type=GPIO_INTR_DISABLE};
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
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_OK && nvs_open("device-pin", NVS_READWRITE, &pin_nvs) == ESP_OK) {
        pin_store_open = true;
        uint8_t reset_done = 0;
        if (nvs_get_u8(pin_nvs, "reset-done", &reset_done) == ESP_OK && reset_done == 1) {
            reset_success_visible = true;
            reset_success_until = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
            if (nvs_erase_key(pin_nvs, "reset-done") == ESP_OK) nvs_commit(pin_nvs);
        }
        size_t size = sizeof(pin_hash);
        if (nvs_get_str(pin_nvs, "bcrypt", pin_hash, &size) != ESP_OK || size != sizeof(pin_hash)) pin_hash[0] = 0;
        if (nvs_get_u32(pin_nvs, "device-id", &device_info_id) != ESP_OK || device_info_id == 0) {
            device_info_id = esp_random();
            if (device_info_id == 0) device_info_id = 1;
            if (nvs_set_u32(pin_nvs, "device-id", device_info_id) != ESP_OK ||
                    nvs_commit(pin_nvs) != ESP_OK) device_info_id = 0;
        }
    }
    if (!pin_hash[0]) {
        pin_mode = PIN_CREATE;
        pin_setup_at_boot = true;
        pin_length = 0;
        selected_key = 0;
        pin_previous_buttons = -1;
    }
    render(false, false);
}
bool wallet_ui_prompt(void) {
    settings_open = false;
    settings_detail_open = false;
    settings_change_confirm = false;
    settings_reset_confirm = false;
    settings_change_success = false;
    history_open = false;
    waiting = true;
    result_visible = false;
    shown_command = atomic_load(&command);
    status = "CONFIRM";
    local_auth_result = 0;
    mbedtls_platform_zeroize(pin_entry, sizeof(pin_entry));
    mbedtls_platform_zeroize(pin_first, sizeof(pin_first));
    pin_length = 0; selected_key = 0; pin_notice = NULL;
    pin_mode = pin_hash[0] ? PIN_VERIFY : PIN_CREATE;
    pin_back_down = false;
    pin_back_cancelled = false;
    pin_previous_buttons = -1;
    previous_buttons = -1;
    return render(!gpio_get_level(GPIO_NUM_5), !gpio_get_level(GPIO_NUM_6));
}
static void pin_clear(void) { mbedtls_platform_zeroize(pin_entry, sizeof(pin_entry)); pin_length = 0; }
static bool pin_calculate(const char *pin, const char *setting, char out[61]) {
    return _crypt_blowfish_rn(pin, setting, out, 61) != NULL;
}
static bool pin_hash_matches(const char *candidate, const char *stored) {
    volatile uint8_t difference = 0;
    for (unsigned i = 0; i < 60; ++i) difference |= (uint8_t)(candidate[i] ^ stored[i]);
    return difference == 0;
}
static bool pin_store_new(const char *new_pin) {
    char salt[30] = {0}, generated[61] = {0};
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    bool ok = _crypt_gensalt_blowfish_rn("$2b$", 10, (const char *)random,
                    sizeof(random), salt, sizeof(salt)) != NULL;
    if (ok) ok = pin_calculate(new_pin, salt, generated);
    if (ok) ok = pin_store_open && nvs_set_str(pin_nvs, "bcrypt", generated) == ESP_OK
                         && nvs_commit(pin_nvs) == ESP_OK;
    if (ok) memcpy(pin_hash, generated, sizeof(pin_hash));
    mbedtls_platform_zeroize(random, sizeof(random));
    mbedtls_platform_zeroize(salt, sizeof(salt));
    mbedtls_platform_zeroize(generated, sizeof(generated));
    return ok;
}
static void reset_challenge_start(void) {
    uint32_t random = esp_random() % 10000;
    snprintf(reset_challenge, sizeof(reset_challenge), "%04lu", (unsigned long)random);
    snprintf(reset_challenge_title, sizeof(reset_challenge_title), "TYPE %s", reset_challenge);
    pin_clear();
    selected_key = 0;
    pin_notice = NULL;
    pin_back_down = false;
    pin_back_cancelled = false;
    pin_previous_buttons = -1;
    pin_mode = PIN_RESET_CHALLENGE;
}
static bool reset_schedule_wipe(void) {
    if (!pin_store_open) return false;
    return nvs_set_u8(pin_nvs, "wipe-pending", 1) == ESP_OK &&
           nvs_commit(pin_nvs) == ESP_OK;
}
static void pin_submit(void) {
    if (pin_mode == PIN_RESET_CHALLENGE) {
        if (pin_length != 4) return;
        bool matched = memcmp(pin_entry, reset_challenge, 4) == 0;
        pin_clear();
        if (matched) {
            pin_mode = PIN_RESET_VERIFY;
            pin_notice = NULL;
        }
        return;
    }
    if (pin_length != 6) return;
    if (pin_mode == PIN_CREATE) {
        memcpy(pin_first, pin_entry, sizeof(pin_first)); pin_clear(); pin_mode = PIN_RETYPE; pin_notice = NULL; return;
    }
    if (pin_mode == PIN_RETYPE) {
        if (memcmp(pin_first, pin_entry, 6) != 0) { pin_clear(); mbedtls_platform_zeroize(pin_first,sizeof(pin_first)); pin_mode=PIN_CREATE; pin_notice="MISMATCH"; selected_key=0; return; }
        bool ok = pin_store_new(pin_entry);
        mbedtls_platform_zeroize(pin_first,sizeof(pin_first)); pin_clear();
        if (!ok) { pin_notice = "FLASH ERROR"; return; }
        pin_mode = PIN_IDLE;
        if (pin_setup_at_boot) {
            pin_setup_at_boot = false;
            waiting = false;
            shown_command = "READY";
            status = "READY";
            atomic_store(&command, "READY");
        } else {
            local_auth_result = 1;
        }
        render(false, false);
        return;
    }
    if (pin_mode == PIN_CHANGE_NEW) {
        memcpy(pin_first, pin_entry, sizeof(pin_first));
        pin_clear();
        pin_mode = PIN_CHANGE_RETYPE;
        pin_notice = NULL;
        return;
    }
    if (pin_mode == PIN_CHANGE_RETYPE) {
        if (memcmp(pin_first, pin_entry, 6) != 0) {
            pin_clear();
            mbedtls_platform_zeroize(pin_first, sizeof(pin_first));
            pin_mode = PIN_CHANGE_NEW;
            pin_notice = "MISMATCH";
            return;
        }
        bool ok = pin_store_new(pin_entry);
        pin_clear();
        mbedtls_platform_zeroize(pin_first, sizeof(pin_first));
        if (!ok) {
            pin_mode = PIN_CHANGE_NEW;
            pin_notice = "FLASH ERROR";
            return;
        }
        pin_mode = PIN_IDLE;
        pin_notice = NULL;
        settings_change_success = true;
        settings_change_success_until = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
        render(false, false);
        return;
    }
    char candidate[61] = {0};
    bool computed = pin_calculate(pin_entry, pin_hash, candidate);
    bool ok = computed && pin_hash_matches(candidate, pin_hash);
    mbedtls_platform_zeroize(candidate,sizeof(candidate)); pin_clear();
    if (pin_mode == PIN_CHANGE_CURRENT) {
        if (ok) {
            pin_mode = PIN_CHANGE_NEW;
            pin_notice = NULL;
            selected_key = 0;
        } else {
            pin_notice = computed ? "WRONG PIN" : "HASH ERROR";
        }
        return;
    }
    if (pin_mode == PIN_RESET_VERIFY) {
        if (!ok) {
            pin_notice = computed ? "WRONG PIN" : "HASH ERROR";
            return;
        }
        if (!reset_schedule_wipe()) {
            pin_notice = "FLASH ERROR";
            return;
        }
        pin_mode = PIN_IDLE;
        settings_open = false;
        shown_command = "RESET DEVICE";
        status = "RESETTING";
        render(false, false);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return;
    }
    if (ok) { pin_mode=PIN_IDLE; local_auth_result=1; } 
}
int wallet_ui_take_local_authorization(void) { int r=local_auth_result; if (r) local_auth_result=0; return r; }
void wallet_ui_result(const char *message) {
    atomic_store(&removal_latched_until, 0);
    waiting = false;
    settings_open = false;
    settings_detail_open = false;
    settings_change_confirm = false;
    settings_reset_confirm = false;
    settings_change_success = false;
    history_open = false;
    pin_mode = PIN_IDLE;
    pin_back_down = false;
    result_visible = true;
    result_until = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    status = strstr(message, "CANCEL") ? "CANCELLED" : message;
    render(false, false);
    previous_buttons = -1;
}
void wallet_ui_command(const char *name) {
    TickType_t now = xTaskGetTickCount();
    if (strcmp(name, "REMOVAL") == 0) {
        // Windows may split allowCredentials across CTAP requests. Keep the
        // removal intent while the following request carries the real key ID.
        atomic_store(&removal_latched_until, now + pdMS_TO_TICKS(15000));
    } else if (strcmp(name, "AUTH") == 0) {
        TickType_t until = atomic_load(&removal_latched_until);
        if (until != 0 && (int32_t)(until - now) > 0) return;
    }
    atomic_store(&command, name);
}
void wallet_ui_task(void) {
    TickType_t now = xTaskGetTickCount();
    if ((TickType_t)(now - last_poll) < pdMS_TO_TICKS(25)) return;
    last_poll = now;
    if (reset_success_visible) {
        if ((int32_t)(now - reset_success_until) >= 0) {
            reset_success_visible = false;
            pin_previous_buttons = -1;
            previous_buttons = -1;
            render(false, false);
        }
        return;
    }
    if (pin_mode != PIN_IDLE) {
        int p = (!gpio_get_level(GPIO_NUM_21)?1:0)|(!gpio_get_level(GPIO_NUM_18)?2:0)|(!gpio_get_level(GPIO_NUM_17)?4:0)|(!gpio_get_level(GPIO_NUM_10)?8:0)|(!gpio_get_level(GPIO_NUM_7)?16:0)|(!gpio_get_level(GPIO_NUM_5)?32:0)|(!gpio_get_level(GPIO_NUM_6)?64:0);
        int newly = p & ~pin_previous_buttons;
        unsigned row = selected_key / 3, col = selected_key % 3;
        if (newly & 1) row=(row+3)%4;
        if (newly & 4) row=(row+1)%4;
        if (newly & 2) col=(col+1)%3;
        if (newly & 8) col=(col+2)%3;
        unsigned candidate=row*3+col;
        if (pin_keys[candidate]==' ') candidate=10;
        selected_key=candidate;
        unsigned required_digits = pin_mode == PIN_RESET_CHALLENGE ? 4 : 6;
        if ((newly & 16) && pin_length < required_digits && pin_keys[selected_key]!=' ') { pin_entry[pin_length++]=pin_keys[selected_key]; pin_entry[pin_length]=0; pin_notice=NULL; }
        if (newly & 32) {
            pin_back_down = true;
            pin_back_cancelled = false;
            pin_back_started = now;
        }
        if (!pin_setup_at_boot && pin_back_down && (p & 32) && !pin_back_cancelled &&
                (TickType_t)(now - pin_back_started) >= pdMS_TO_TICKS(1000)) {
            pin_back_cancelled = true;
            if (settings_open) {
                pin_mode = PIN_IDLE;
                settings_detail_open = false;
                settings_change_confirm = false;
                settings_reset_confirm = false;
                pin_notice = NULL;
                suppress_settings_back_release = true;
            } else {
                local_auth_result = -1;
            }
            mbedtls_platform_zeroize(pin_entry, sizeof(pin_entry));
            mbedtls_platform_zeroize(pin_first, sizeof(pin_first));
            pin_length = 0;
        }
        if (pin_back_down && !(p & 32)) {
            if (!pin_back_cancelled && pin_length) pin_entry[--pin_length]=0;
            pin_back_down = false;
        }
        if (newly & 64) pin_submit();
        pin_previous_buttons=p;
        render((p & 32) != 0, (p & 64) != 0);
        return;
    }
    int pins = (gpio_get_level(GPIO_NUM_5) ? 1 : 0) |
               (gpio_get_level(GPIO_NUM_6) ? 2 : 0) |
               (gpio_get_level(GPIO_NUM_21) ? 4 : 0) |
               (gpio_get_level(GPIO_NUM_17) ? 8 : 0) |
               (gpio_get_level(GPIO_NUM_7) ? 16 : 0);
    if (settings_change_success && (int32_t)(now - settings_change_success_until) >= 0) {
        settings_change_success = false;
        settings_detail_open = false;
        render(false, false);
        previous_buttons = pins;
        return;
    }
    if (!waiting && result_visible &&
            (int32_t)(now - result_until) >= 0) {
        result_visible = false;
        settings_open = false;
        settings_detail_open = false;
        settings_change_confirm = false;
        settings_reset_confirm = false;
        settings_change_success = false;
        history_open = false;
        shown_command = "READY";
        status = "READY";
        atomic_store(&removal_latched_until, 0);
        atomic_store(&command, "READY");
        render(false, false);
        previous_buttons = pins;
        return;
    }
    const char *next = atomic_load(&command);
    // Preserve the last operation/result through background GET INFO traffic.
    if (!waiting && !result_visible && next != shown_command &&
            strcmp(next, "GET INFO") != 0) {
        settings_open = false;
        settings_detail_open = false;
        settings_change_confirm = false;
        settings_reset_confirm = false;
        settings_change_success = false;
        history_open = false;
        shown_command = next;
        status = "WORKING";
        previous_buttons = -1;
    }
    if (!waiting && !result_visible && settings_open && settings_reset_confirm &&
            strcmp(status, "READY") == 0) {
        if (gpio_get_level(GPIO_NUM_7)) reset_yes_armed = true;
        else if (reset_yes_armed) {
            reset_yes_armed = false;
            settings_reset_confirm = false;
            reset_challenge_start();
            previous_buttons = pins;
            render((pins & 1) == 0, (pins & 2) == 0);
            return;
        }
    }
    if (!waiting && !result_visible && strcmp(status, "READY") == 0 && previous_buttons >= 0) {
        if (settings_open) {
            if ((pins & 1) && !(previous_buttons & 1)) {
                if (suppress_settings_back_release) suppress_settings_back_release = false;
                else if (settings_detail_open) {
                    settings_detail_open = false;
                    settings_change_confirm = false;
                    settings_reset_confirm = false;
                    settings_change_success = false;
                }
                else settings_open = false;
            } else if (settings_change_confirm && !(pins & 16) && (previous_buttons & 16)) {
                settings_change_confirm = false;
                pin_mode = PIN_CHANGE_CURRENT;
                pin_notice = NULL;
                pin_clear();
                mbedtls_platform_zeroize(pin_first, sizeof(pin_first));
                selected_key = 0;
                pin_back_down = false;
                pin_back_cancelled = false;
                pin_previous_buttons = -1;
            } else if (settings_detail_open && settings_selection == 0 &&
                       !(pins & 16) && (previous_buttons & 16)) {
                device_info_page = 1 - device_info_page;
            } else if (!settings_detail_open) {
                if (!(pins & 4) && (previous_buttons & 4))
                    settings_selection = (settings_selection + 2) % 3;
                else if (!(pins & 8) && (previous_buttons & 8))
                    settings_selection = (settings_selection + 1) % 3;
                else if (!(pins & 16) && (previous_buttons & 16)) {
                    settings_detail_open = true;
                    device_info_page = 0;
                    if (settings_selection == 1) settings_change_confirm = true;
                    if (settings_selection == 2) {
                        settings_reset_confirm = true;
                        reset_yes_armed = false;
                    }
                }
            }
        } else if ((pins & 1) && !(previous_buttons & 1) && !history_open) {
            settings_open = true;
            settings_selection = 0;
            settings_detail_open = false;
        } else if ((pins & 2) && !(previous_buttons & 2) && !settings_open) {
            history_open = !history_open;
        }
    }
    if (pins != previous_buttons) {
        render((pins & 1) == 0, (pins & 2) == 0);
        previous_buttons = pins;
    }
}
