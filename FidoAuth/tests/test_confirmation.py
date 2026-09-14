"""Host-test the actual button wait functions with fake GPIO, time and OLED."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'pico-keys-sdk/src/button.c').read_text()
functions = source[source.index('void button_wait_start(void)'):source.index('\n#endif\n\nvoid button_task')]
reader = source[source.index('static bool picok_board_button_read(void)'):source.index('#elif defined(PICO_PLATFORM)', source.index('static bool picok_board_button_read(void)'))]
prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#define ESP_PLATFORM 1
#define GPIO_NUM_5 5
#define GPIO_NUM_6 6
#define MODE_BUTTON 1
#define SIGNAL_USER_PRESENCE_COMPLETED 1
#define SIGNAL_USER_PRESENCE_REQUEST 2
#define SIGNAL_USER_PRESENCE_TIMEOUT 3
#define SIGNAL_USER_PRESENCE_CANCELLED 4
#define EV_BUTTON_PRESSED 1
#define EV_BUTTON_TIMEOUT 2
#define EV_BUTTON_CANCELLED 3
typedef enum { BUTTON_EV_PRESSED, BUTTON_EV_TIMEOUT, BUTTON_EV_CANCELLED, BUTTON_EV_NONE } button_event_t;
typedef struct { uint32_t timeout; } signal_user_presence_request_data_t;
static struct { bool up_btn_present; uint32_t up_btn; } phy_data;
static bool cancel_button, force_button_wait, async_button_wait, async_button_pressed;
static bool approve_armed, approve_raw, approve_stable, cancel_raw, req_button_pending, physical_cancel;
static uint32_t approve_changed, cancel_changed, async_button_started, async_button_timeout, async_button_led_mode;
static uint32_t now, event;
static int usb_to_card_q;
static bool ok, cancel, display_ok = true;
static uint32_t board_millis(void) { return now; }
static int gpio_get_level(int pin) { return pin == GPIO_NUM_5 ? ok : cancel; }
static bool wallet_ui_prompt(void) { return display_ok; }
static void wallet_ui_result(const char *s) { (void)s; }
static void signal_emit(int s) { (void)s; }
static void signal_emit_param(int s, void *p) { (void)s; (void)p; }
static void queue_try_add(int *q, uint32_t *e) { (void)q; event=*e; }
static uint32_t led_get_mode(void) { return 0; }
static void led_set_mode(uint32_t m) { (void)m; }
'''
tests = r'''
static void tick(uint32_t ms) { now += ms; button_wait_poll(); }
static void start(bool held) { ok=held; cancel=false; cancel_button=false; event=0; display_ok=true; button_wait_start(); }
static void approve(void) { ok=true; tick(1); tick(30); ok=false; tick(1); tick(30); }
int main(void) {
 start(false); tick(30); assert(event==0); approve(); assert(event==EV_BUTTON_PRESSED);
 start(true); tick(40); ok=false; tick(1); tick(30); assert(event==0); approve(); assert(event==EV_BUTTON_PRESSED);
 start(false); cancel=true; tick(1); tick(30); assert(event==EV_BUTTON_CANCELLED);
 start(false); tick(30000); assert(event==EV_BUTTON_TIMEOUT);
 start(false); tick(30); ok=true; tick(1); tick(10); ok=false; tick(1); tick(30); assert(event==0);
 start(false); tick(30); ok=true; tick(1); tick(30); ok=false; cancel=true; tick(1); tick(30); assert(event==EV_BUTTON_CANCELLED);
 start(false); display_ok=false; button_wait_start(); tick(30); assert(event==0); approve(); assert(event==EV_BUTTON_PRESSED);
 start(false); cancel_button=true; button_wait_start(); tick(1); assert(event==EV_BUTTON_CANCELLED);
 now=UINT32_MAX-100; start(false); tick(30000); assert(event==EV_BUTTON_TIMEOUT);
 return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'confirmation.c'
    binary = Path(tmp) / 'confirmation'
    c.write_text(prefix + reader + functions + tests)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: approval, held button, cancellation, timeout, bounce, simultaneous buttons, OLED failure, clock rollover')
