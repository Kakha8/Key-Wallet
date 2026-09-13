#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <nvs.h>
#include <esp_random.h>
#include <bootloader_random.h>
#include <esp_task_wdt.h>
#include <string.h>

extern "C" {
#include "crypt_blowfish.h"
}

SET_LOOP_TASK_STACK_SIZE(16384);

constexpr int SDA_PIN = 11, SCL_PIN = 12;
constexpr uint8_t BCRYPT_COST = 10; // Each +1 roughly doubles hashing time.
static_assert(BCRYPT_COST >= 4 && BCRYPT_COST <= 14, "Use a cost from 4 to 14");
Adafruit_SSD1306 display(128, 64, &Wire, -1);

enum ButtonIndex { BTN_UP, BTN_RIGHT, BTN_DOWN, BTN_LEFT, BTN_OK,
                   BTN_DELETE, BTN_SUBMIT, BTN_COUNT };
struct Button {
  uint8_t pin;
  bool lastReading, stableState;
  unsigned long changedAt;
};
bool buttonPressed(Button& button); // Prevent Arduino from generating this above the struct.
Button buttons[] = {
  {21, HIGH, HIGH, 0}, {18, HIGH, HIGH, 0},
  {17, HIGH, HIGH, 0}, {10, HIGH, HIGH, 0},
  { 7, HIGH, HIGH, 0}, { 5, HIGH, HIGH, 0}, { 6, HIGH, HIGH, 0}
};

enum ScreenMode { CREATE_PIN, RETYPE_PIN, AUTH_PIN, UNLOCKED, FATAL_ERROR };
ScreenMode mode = FATAL_ERROR;
const char KEYS[] = "123456789 0 "; // Empty bottom corners are not selectable.
const char* heading = "STARTING";
int selectedKey = 0;
char enteredPin[7] = {}, firstPin[7] = {}, savedHash[61] = {};
uint8_t pinLength = 0;
nvs_handle_t storage;
unsigned long retryStarted = 0;
bool retryWait = false;

// Called by the bundled bcrypt C source periodically, including at high costs.
extern "C" void pin_bcrypt_yield(void) {
  if (esp_task_wdt_status(NULL) == ESP_OK) esp_task_wdt_reset();
  vTaskDelay(1); // Give idle tasks CPU time without disabling the watchdog.
}

void wipe(void* memory, size_t size) {
  volatile uint8_t* p = static_cast<volatile uint8_t*>(memory);
  while (size--) *p++ = 0;
}

bool equalBytes(const char* a, const char* b, size_t length) {
  volatile uint8_t difference = 0;
  for (size_t i = 0; i < length; ++i) difference |= a[i] ^ b[i];
  return difference == 0;
}

void clearEntry() {
  wipe(enteredPin, sizeof(enteredPin));
  pinLength = 0;
}

bool buttonPressed(Button& button) {
  bool reading = digitalRead(button.pin);
  unsigned long now = millis();
  if (reading != button.lastReading) {
    button.lastReading = reading;
    button.changedAt = now;
  }
  if (now - button.changedAt >= 30 && reading != button.stableState) {
    button.stableState = reading;
    return reading == LOW;
  }
  return false;
}

void drawPinScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setTextWrap(false);
  display.setCursor((128 - strlen(heading) * 6) / 2, 2);
  display.print(heading);
  display.drawFastHLine(0, 13, 128, SSD1306_WHITE);
  if (mode == UNLOCKED || mode == FATAL_ERROR) {
    display.setCursor(10, 31);
    display.print(mode == UNLOCKED ? "OK to lock" : "Restart to retry");
  } else {
    for (int i = 0; i < 6; ++i) {
      int x = 1 + i * 12;
      display.drawRoundRect(x, 29, 10, 18, 2, SSD1306_WHITE);
      if (i < pinLength) display.fillCircle(x + 5, 38, 2, SSD1306_WHITE);
    }
    for (int i = 0; i < 12; ++i) {
      if (KEYS[i] == ' ') continue;
      int x = 76 + (i % 3) * 17, y = 17 + (i / 3) * 12;
      bool selected = i == selectedKey;
      if (selected) display.fillRoundRect(x, y, 15, 11, 2, SSD1306_WHITE);
      else display.drawRoundRect(x, y, 15, 11, 2, SSD1306_WHITE);
      display.setTextColor(selected ? SSD1306_BLACK : SSD1306_WHITE);
      display.setCursor(x + 5, y + 2);
      display.write(KEYS[i]);
    }
  }
  display.setTextColor(SSD1306_WHITE);
  display.display();
}

void fatalError(const char* message) {
  clearEntry();
  wipe(firstPin, sizeof(firstPin));
  mode = FATAL_ERROR;
  heading = message;
  drawPinScreen();
}

const char* entryHeading() {
  return mode == CREATE_PIN ? "CREATE PIN" :
         mode == RETYPE_PIN ? "RETYPE PIN" : "ENTER PIN";
}

bool validStoredHash(const char* hash) {
  if (strlen(hash) != 60 || strncmp(hash, "$2b$", 4) != 0 || hash[6] != '$') return false;
  if (hash[4] < '0' || hash[4] > '9' || hash[5] < '0' || hash[5] > '9') return false;
  int cost = (hash[4] - '0') * 10 + hash[5] - '0';
  if (cost < 4 || cost > 14) return false;
  for (int i = 7; i < 60; ++i) {
    char c = hash[i];
    if (!(c == '.' || c == '/' || (c >= '0' && c <= '9') ||
          (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
  }
  return true;
}

bool calculateHash(const char* pin, const char* setting, char* result) {
  unsigned long started = millis();
  bool success = _crypt_blowfish_rn(pin, setting, result, 61) != nullptr;
  Serial.printf("bcrypt cost %c%c: %lu ms (%s)\n", setting[4], setting[5],
                millis() - started, success ? "computed" : "ERROR");
  return success;
}

void submitPin() {
  if (pinLength != 6) { heading = "NEED 6 DIGITS"; return; }
  if (mode == CREATE_PIN) {
    memcpy(firstPin, enteredPin, sizeof(firstPin));
    clearEntry();
    mode = RETYPE_PIN;
    heading = "RETYPE PIN";
    selectedKey = 0;
    return;
  }
  if (mode == RETYPE_PIN) {
    if (!equalBytes(firstPin, enteredPin, 6)) {
      clearEntry();
      wipe(firstPin, sizeof(firstPin));
      mode = CREATE_PIN;
      heading = "MISMATCH: NEW PIN";
      selectedKey = 0;
      return;
    }
    heading = "SAVING...";
    drawPinScreen();
    char salt[30] = {}, newHash[61] = {};
    uint32_t randomSalt[4];
    // This sketch does not use Wi-Fi, Bluetooth, or ADC.
    bootloader_random_enable();
    esp_fill_random(randomSalt, sizeof(randomSalt));
    bootloader_random_disable();
    bool success = _crypt_gensalt_blowfish_rn("$2b$", BCRYPT_COST,
       reinterpret_cast<const char*>(randomSalt), sizeof(randomSalt),
       salt, sizeof(salt)) != nullptr;
    if (success) success = calculateHash(enteredPin, salt, newHash);
    wipe(randomSalt, sizeof(randomSalt));
    clearEntry();
    wipe(firstPin, sizeof(firstPin));
    if (!success) { wipe(newHash, sizeof(newHash)); fatalError("HASH ERROR"); return; }
    esp_err_t error = nvs_set_str(storage, "hash", newHash);
    if (error == ESP_OK) error = nvs_commit(storage);
    size_t size = sizeof(savedHash);
    if (error == ESP_OK) error = nvs_get_str(storage, "hash", savedHash, &size);
    bool saved = error == ESP_OK && equalBytes(savedHash, newHash, sizeof(savedHash));
    wipe(newHash, sizeof(newHash));
    if (!saved) { fatalError("FLASH ERROR"); return; }
    mode = AUTH_PIN;
    heading = "SAVED: ENTER PIN";
    selectedKey = 0;
    return;
  }
  if (mode == AUTH_PIN) {
    heading = "CHECKING...";
    drawPinScreen();
    char candidate[61] = {};
    bool success = calculateHash(enteredPin, savedHash, candidate);
    bool matches = success && equalBytes(candidate, savedHash, 60);
    wipe(candidate, sizeof(candidate));
    clearEntry();
    if (!success) { fatalError("HASH ERROR"); return; }
    if (matches) {
      mode = UNLOCKED;
      heading = "ACCESS GRANTED";
      // Add your authenticated application here.
    } else {
      heading = "WRONG PIN: WAIT";
      retryStarted = millis();
      retryWait = true;
    }
  }
}

void activateSelectedKey() {
  char key = KEYS[selectedKey];
  if (key >= '0' && key <= '9') {
    if (pinLength < 6) {
      enteredPin[pinLength++] = key;
      enteredPin[pinLength] = '\0';
    }
    heading = entryHeading();
  }
}

void moveSelection(int rowStep, int colStep) {
  int row = selectedKey / 3, col = selectedKey % 3;
  do {
    row = (row + rowStep + 4) % 4;
    col = (col + colStep + 3) % 3;
  } while (KEYS[row * 3 + col] == ' ');
  selectedKey = row * 3 + col;
}

void setup() {
  Serial.begin(115200);
  for (Button& button : buttons) pinMode(button.pin, INPUT_PULLUP);
  if (!Wire.begin(SDA_PIN, SCL_PIN)) { while (true) delay(100); }
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C, true, false)) {
    while (true) delay(100);
  }
  // Arduino initializes NVS. Never erase storage automatically on an error.
  if (nvs_open("pin-auth", NVS_READWRITE, &storage) != ESP_OK) {
    fatalError("FLASH ERROR"); return;
  }
  size_t size = sizeof(savedHash);
  esp_err_t error = nvs_get_str(storage, "hash", savedHash, &size);
  if (error == ESP_ERR_NVS_NOT_FOUND) {
    mode = CREATE_PIN;
  } else if (error == ESP_OK && validStoredHash(savedHash)) {
    mode = AUTH_PIN;
  } else {
    fatalError("STORED HASH ERROR"); return;
  }
  heading = entryHeading();
  drawPinScreen();
}

void loop() {
  bool pressed[BTN_COUNT];
  for (int i = 0; i < BTN_COUNT; ++i) pressed[i] = buttonPressed(buttons[i]);
  if (mode == FATAL_ERROR) { delay(5); return; }
  if (retryWait) {
    if (millis() - retryStarted >= 3000) {
      retryWait = false;
      heading = "ENTER PIN";
      drawPinScreen();
    }
    delay(5); return;
  }
  if (mode == UNLOCKED) {
    if (pressed[BTN_OK]) {
      mode = AUTH_PIN;
      heading = "ENTER PIN";
      selectedKey = 0;
      drawPinScreen();
    }
    delay(5); return;
  }
  bool redraw = false;
  if (pressed[BTN_UP])    { moveSelection(-1, 0); redraw = true; }
  if (pressed[BTN_RIGHT]) { moveSelection(0, 1); redraw = true; }
  if (pressed[BTN_DOWN])  { moveSelection(1, 0); redraw = true; }
  if (pressed[BTN_LEFT])  { moveSelection(0, -1); redraw = true; }
  // Process only one entry action per scan: delete, submit, or digit selection.
  if (pressed[BTN_DELETE]) {
    if (pinLength) enteredPin[--pinLength] = '\0';
    heading = entryHeading();
    redraw = true;
  } else if (pressed[BTN_SUBMIT]) {
    submitPin();
    redraw = true;
  } else if (pressed[BTN_OK]) { activateSelectedKey(); redraw = true; }
  if (redraw) drawPinScreen();
  delay(1);
}
