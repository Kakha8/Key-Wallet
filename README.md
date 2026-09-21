# Enigma Key Wallet

Enigma Key Wallet is an ESP32-S3 hardware authenticator project. It combines a
standards-based FIDO2/WebAuthn security key with a local OLED interface, a
device PIN, physical controls, retry lockouts, device settings, and secure
credential storage provided by the Pico FIDO/Pico Keys codebase.

The target is an ESP32-S3 Nano-style board with native USB. The firmware
enumerates as a USB HID FIDO authenticator and works with operating systems and
browsers through their normal WebAuthn/security-key interfaces. The upstream
FIDO implementation and its USB VID/PID are intentionally preserved.

> This repository contains the device firmware. The application backend is
> maintained separately in `File-Drive-Spring`.

## Repository layout

- `FidoAuth/` — the main ESP-IDF FIDO2 firmware, based on Pico FIDO and the
  bundled Pico Keys SDK.
- `FidoAuth/src/fido/` — CTAP/FIDO command processing and project-specific
  behavior.
- `FidoAuth/pico-keys-sdk/src/wallet_ui.c` — OLED rendering, physical-button
  input, device-PIN workflows, settings, reset, operation history, and lockout
  handling.
- `FidoAuth/pico-keys-sdk/src/button.c` — bridges local authorization into the
  FIDO user-presence event flow.
- `PinAuth/` — the earlier standalone Arduino PIN-entry prototype. It is useful
  as a hardware/UI reference but is not the primary FIDO firmware.

## Hardware

### Main board

- ESP32-S3 with native USB
- The board's boot/download GPIO remains separate from the application controls
- Native USB is used for CTAP HID communication

### OLED

The display is an SSD1306 128x64 OLED on I2C bus 0:

| Signal | Connection |
| --- | --- |
| Address | `0x3C` |
| SDA | GPIO11 |
| SCL | GPIO12 |

### Controls

All controls use internal pull-ups and are active-low: pressing a button
connects its GPIO to GND.

| Action | GPIO |
| --- | ---: |
| Up | 21 |
| Right | 18 |
| Down | 17 |
| Left | 10 |
| Select / OK | 7 |
| Back / Delete | 5 |
| Submit | 6 |

### Real-time clock

A DS3231-compatible RTC is used by the persistent PIN retry-lockout mechanism.
It is connected to I2C bus 1:

| Signal | Connection |
| --- | --- |
| Address | `0x68` |
| SDA | GPIO8 |
| SCL | GPIO9 |

The firmware fails closed with an `RTC ERROR` if reliable time is required for
PIN verification but cannot be obtained. Moving the RTC backwards is also
rejected by comparing it with the last persisted timestamp.

## How authentication works

1. A browser or operating system sends a WebAuthn operation to the device over
   native USB using CTAP HID.
2. The Pico FIDO command layer parses the request and labels the operation for
   the local UI, such as `REGISTER`, `AUTH`, `REMOVE DEVICE`, `RESET`, or
   `PIN COMMAND`.
3. When user presence is required, `button_wait_start()` opens the local
   authorization screen and starts a 30-second wait.
4. The user enters the six-digit device PIN using the directional keypad,
   Select, Back/Delete, and Submit controls.
5. The firmware verifies the local PIN. A successful local authorization is
   converted into the existing upstream FIDO button/user-presence event.
6. The FIDO implementation completes the WebAuthn request and the OLED displays
   the result before returning to the ready screen.

The local OLED PIN is an additional device-side authorization gate. It should
not be confused with the CTAP security-key PIN entered by Windows or the
browser. The host PIN belongs to the standard FIDO protocol; the local PIN is
verified entirely by the firmware UI.

## Device PIN

On first boot, the OLED requires creation and confirmation of a six-digit local
PIN. The firmware:

- hashes the PIN with bcrypt at cost 10;
- stores only the bcrypt hash in ESP32 NVS;
- uses random salt material from the ESP32 random source;
- compares hashes without an early-exit byte comparison;
- clears transient PIN and hash buffers after use where practical; and
- requires the local PIN before approving protected FIDO operations.

PIN creation, verification, change, and device-reset verification all use the
same on-device keypad. Holding Back for one second cancels an active local
authorization flow; a short Back press deletes one entered digit.

### Retry lockout

Incorrect local PIN attempts are persisted in NVS. The current delays are:

| Failed attempt | Lockout |
| ---: | ---: |
| 1–4 | No timed delay |
| 5 | 30 seconds |
| 6 | 2 minutes |
| 7 | 10 minutes |
| 8 or more | 1 hour |

A correct PIN resets the failure count. The failure count, lockout deadline,
and last trusted RTC timestamp survive restarts.

## OLED interface

The OLED is driven directly by the firmware and shows:

- startup PIN creation when no local PIN exists;
- the ready screen and project logo;
- the current FIDO operation;
- PIN entry and confirmation screens;
- approval, cancellation, timeout, and error states;
- device information and USB/PIN status;
- operation history; and
- settings for changing the PIN or resetting the device.

Background CTAP `GET INFO` traffic is intentionally prevented from replacing a
meaningful foreground operation or its result.

## Settings and reset

The settings interface exposes device information, PIN change, and device
reset. A reset is deliberately multi-step:

1. Confirm the reset action.
2. Type the randomly generated four-digit challenge shown on the OLED.
3. Enter the current local PIN.
4. The firmware records a pending wipe and restarts.
5. Before USB and normal FIDO tasks start, the target data partition and NVS
   are erased, then the device restarts into fresh setup.

This sequence reduces accidental resets and performs the destructive erase
before the normal authenticator becomes available.

## FIDO foundation

`FidoAuth/` retains the upstream Pico FIDO/Pico Keys implementation rather than
replacing it with a custom authenticator. It provides the CTAP2/WebAuthn
protocol, resident credentials, credential management, PIN/UV support,
extensions, USB HID transport, and encrypted device storage facilities. See
[`FidoAuth/README.md`](FidoAuth/README.md) for the complete upstream feature
list and its build/test documentation.

The configured USB identity remains:

- VID: `0x2E8A`
- PID: `0x10FE`

Do not change these values casually: host recognition, interoperability, and
distribution rights depend on the USB identity and descriptors remaining
consistent.

## Building and flashing

The main firmware is an ESP-IDF project. With a compatible ESP-IDF environment
activated:

```sh
cd FidoAuth
idf.py set-target esp32s3
idf.py build
idf.py -p COM_PORT flash monitor
```

Replace `COM_PORT` with the board's serial port. After flashing, reconnect the
native USB connection and verify that Windows and the browser recognize the
device as a security key.

## Planned cryptographic wallet work

The next major goal is to extend the authenticator into a client-side
encryption and signing device. This work is planned and is **not yet described
as production-ready functionality** in this repository.

The intended design includes:

- secure generation and storage of a key-encryption key (KEK);
- secure generation and storage of a dedicated signing key;
- envelope encryption, with independently generated data-encryption keys
  wrapped by the device-held KEK;
- client-side file encryption so plaintext and unwrapped data keys do not need
  to be exposed to the storage backend;
- ML-KEM-1024 for post-quantum key encapsulation and recipient key wrapping;
- ML-DSA-87 for post-quantum signatures over encrypted objects, manifests, or
  other protocol-bound metadata;
- explicit key identifiers, versions, algorithms, usage policy, and lifecycle
  state for rotation and migration;
- user-presence and local-PIN authorization before sensitive unwrap or signing
  operations; and
- a versioned envelope format with authenticated metadata, algorithm agility,
  replay/context binding, and clear separation between encryption and signing
  keys.

A likely high-level client-side flow is:

1. The client generates a random symmetric data-encryption key (DEK).
2. The client encrypts the file with an authenticated symmetric cipher.
3. ML-KEM-1024 establishes/wraps the key material needed by an authorized
   recipient, while the device KEK protects locally retained key material.
4. The device signs the canonical envelope metadata and ciphertext digest with
   ML-DSA-87 after local authorization.
5. The backend stores ciphertext, wrapped-key material, metadata, and the
   signature, but does not receive plaintext or an unwrapped DEK.
6. During decryption, the client verifies the signature and envelope context,
   obtains device authorization, unwraps/decapsulates the DEK, and decrypts the
   file locally.

Before implementation, this protocol needs a precise threat model and binary
format. In particular, the design must define which component owns each private
key, how ML-KEM and the KEK interact without redundant or unsafe wrapping,
which metadata is authenticated as associated data, how rollback is detected,
and how recovery and key rotation work.

## Security status

This is security-sensitive firmware under active development. Building a
working prototype does not by itself establish resistance to physical attacks,
side channels, malicious firmware, rollback, fault injection, or supply-chain
compromise. Production use should include secure boot, flash encryption,
protected root-key provisioning, signed updates, rollback protection, protocol
review, and independent security testing.

## License and upstream attribution

The FIDO firmware is derived from Pico FIDO and Pico Keys SDK. Their source
headers and [`FidoAuth/README.md`](FidoAuth/README.md) describe the applicable
AGPLv3 licensing, upstream credits, and commercial licensing options. Preserve
copyright, license, and attribution notices when modifying or distributing the
firmware.
