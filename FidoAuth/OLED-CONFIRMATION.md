# OLED confirmation

This ESP32 build requires physical confirmation for user-presence requests.
The browser still manages the FIDO security-key PIN.

Wiring uses raw ESP32 GPIO numbers, not Nano D-pin labels:

| Component | GPIO |
| --- | --- |
| SSD1306 128x64 SDA | 11 |
| SSD1306 SCL | 12 |
| OK button, HIGH when pressed | 5 |
| Cancel button, HIGH when pressed | 6 |

OLED address is 0x3C. Button pull-downs are enabled internally. Both buttons
are active-high: released LOW, pressed HIGH (3.3V logic).
The screen shows CONFIRM AUTH / OK TO APPROVE / LEFT TO CANCEL for
FIDO user-presence requests, including enrollment. Press and release OK
to approve. Cancel rejects the pending request. The request times out
after 30 seconds. A button already held when the prompt appears must
be released before a new press can approve. OLED write failures do not
cancel the request; explicit physical approval is still required.

The current display header is KEY WALLET REV E. This revision scopes host
cancellation to the active channel and lets the worker complete its response
without clearing shared response buffers from the USB callback. CBOR request
bytes are copied before the worker starts so incoming CANCEL packets cannot
overwrite them. Host cancellation remains effective before the prompt starts.

The Windows enrollment flow still needs verification on the physical board.

REV C shows REGISTER, AUTH, SELECT, RESET, PIN COMMAND or PIN SET when
those commands are processed. PIN SET means the request arrived, not that
the PIN was saved successfully. Confirmation results distinguish HOST CANCEL,
GPIO FIVE CANCEL, TIMED OUT and APPROVED.

During confirmation, OK and CANCEL appear as outlined on-screen buttons.
Pressing the matching physical button fills its rectangle and reverses the
label color. The idle screen only shows KEY WALLET and READY; raw GPIO and
diagnostic text are hidden.

APPROVED means physical consent was given; the browser/server still
decides whether enrollment or authentication succeeded.

## Hardware verification after flashing

1. Check READY on the OLED and that Windows still recognizes the key.
2. Start enrollment or login and check that it waits at CONFIRM AUTH.
3. Press and release GPIO7: the operation should continue.
4. Repeat and press GPIO5: the operation must fail without authenticating.
5. Repeat without pressing either button: it must time out.
6. Hold OK before starting: releasing it must not approve; press again.

The implementation is inside the nested pico-keys-sdk repository. Commit
its changes (including wallet_ui.c and wallet_ui.h) in that repository
and preserve the corresponding submodule revision when sharing the build.
