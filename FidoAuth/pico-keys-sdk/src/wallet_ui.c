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
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_random.h"
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
static TickType_t result_until;
static int previous_buttons = -1;
static TickType_t last_poll;
typedef enum { PIN_IDLE, PIN_CREATE, PIN_RETYPE, PIN_VERIFY, PIN_HASHING } pin_mode_t;
static pin_mode_t pin_mode;
static int local_auth_result;
static char pin_entry[7], pin_first[7], pin_hash[61];
static unsigned pin_length, selected_key;
static const char pin_keys[] = "123456789 0 ";
static const char *pin_notice;
static TickType_t pin_back_started;
static bool pin_back_down;
static bool pin_back_cancelled;
static nvs_handle_t pin_nvs;
static bool pin_store_open;
static int pin_previous_buttons = -1;
static bool pin_setup_at_boot;
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
static void pin_button(unsigned x, unsigned w, const char *label, bool pressed) {
    const unsigned y = 50, h = 13;
    if (pressed) fill_rect(x, y, w, h, true); else outline(x, y, w, h);
    size_t label_width = strlen(label) * 6;
    text(x + (w - label_width) / 2, y + 3, label, !pressed);
}
static const char *pin_title(void) {
    if (pin_notice) return pin_notice;
    if (pin_mode == PIN_CREATE) return "CREATE PIN";
    if (pin_mode == PIN_RETYPE) return "RETYPE PIN";
    if (strcmp(shown_command, "REGISTER") == 0) return "REGISTER PIN";
    if (strcmp(shown_command, "REMOVAL") == 0) return "REMOVE DEVICE";
    return "AUTH PIN";
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
    if (pin_mode != PIN_IDLE) {
        centered(1, pin_title(), true);
        for (unsigned i = 0; i < 6; ++i) {
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
        size_t size = sizeof(pin_hash);
        if (nvs_get_str(pin_nvs, "bcrypt", pin_hash, &size) != ESP_OK || size != sizeof(pin_hash)) pin_hash[0] = 0;
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
    waiting = true;
    result_visible = false;
    shown_command = atomic_load(&command);
    status = "CONFIRM";
    local_auth_result = 0;
    pin_length = 0; pin_entry[0] = 0; selected_key = 0; pin_notice = NULL;
    pin_mode = pin_hash[0] ? PIN_VERIFY : PIN_CREATE;
    pin_back_down = false;
    pin_back_cancelled = false;
    pin_previous_buttons = -1;
    previous_buttons = -1;
    return render(gpio_get_level(GPIO_NUM_5), gpio_get_level(GPIO_NUM_6));
}
static void pin_clear(void) { mbedtls_platform_zeroize(pin_entry, sizeof(pin_entry)); pin_length = 0; }
static bool pin_calculate(const char *pin, const char *setting, char out[61]) {
    return _crypt_blowfish_rn(pin, setting, out, 61) != NULL;
}
static void pin_submit(void) {
    if (pin_length != 6) return;
    if (pin_mode == PIN_CREATE) {
        memcpy(pin_first, pin_entry, sizeof(pin_first)); pin_clear(); pin_mode = PIN_RETYPE; pin_notice = NULL; return;
    }
    if (pin_mode == PIN_RETYPE) {
        if (memcmp(pin_first, pin_entry, 6) != 0) { pin_clear(); mbedtls_platform_zeroize(pin_first,sizeof(pin_first)); pin_mode=PIN_CREATE; pin_notice="MISMATCH"; selected_key=0; return; }
        char salt[30] = {0}, generated[61] = {0}; uint8_t random[16]; esp_fill_random(random,sizeof(random));
        bool ok = _crypt_gensalt_blowfish_rn("$2b$",10,(const char *)random,sizeof(random),salt,sizeof(salt)) && pin_calculate(pin_entry,salt,generated);
        mbedtls_platform_zeroize(random,sizeof(random));
        if (ok && pin_store_open && nvs_set_str(pin_nvs,"bcrypt",generated)==ESP_OK && nvs_commit(pin_nvs)==ESP_OK) memcpy(pin_hash,generated,sizeof(pin_hash)); else ok=false;
        mbedtls_platform_zeroize(generated,sizeof(generated)); mbedtls_platform_zeroize(pin_first,sizeof(pin_first)); pin_clear();
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
    char candidate[61] = {0}; bool ok = pin_calculate(pin_entry,pin_hash,candidate) && memcmp(candidate,pin_hash,60)==0;
    mbedtls_platform_zeroize(candidate,sizeof(candidate)); pin_clear();
    if (ok) { pin_mode=PIN_IDLE; local_auth_result=1; } 
}
int wallet_ui_take_local_authorization(void) { int r=local_auth_result; if (r) local_auth_result=0; return r; }
void wallet_ui_result(const char *message) {
    atomic_store(&removal_latched_until, 0);
    waiting = false;
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
    if ((TickType_t)(now - last_poll) < pdMS_TO_TICKS(100)) return;
    last_poll = now;
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
        if ((newly & 16) && pin_length<6 && pin_keys[selected_key]!=' ') { pin_entry[pin_length++]=pin_keys[selected_key]; pin_entry[pin_length]=0; pin_notice=NULL; }
        if (newly & 32) {
            pin_back_down = true;
            pin_back_cancelled = false;
            pin_back_started = now;
        }
        if (!pin_setup_at_boot && pin_back_down && (p & 32) && !pin_back_cancelled &&
                (TickType_t)(now - pin_back_started) >= pdMS_TO_TICKS(1000)) {
            pin_back_cancelled = true;
            local_auth_result = -1;
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
               (gpio_get_level(GPIO_NUM_6) ? 2 : 0);
    if (!waiting && result_visible &&
            (int32_t)(now - result_until) >= 0) {
        result_visible = false;
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
        shown_command = next;
        status = "WORKING";
        previous_buttons = -1;
    }
    if (pins != previous_buttons) {
        render((pins & 1) != 0, (pins & 2) != 0);
        previous_buttons = pins;
    }
}
