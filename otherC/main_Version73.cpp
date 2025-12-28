/*
  main.cpp (v73)

  v73: RMT-based PSYN — precise synthesized PWM on GPIO21 (21.5 kHz) with optional fixed duty.
  Fixes: restore all v71 implementations that were accidentally omitted; ensure RMT PSYN task
  cannot starve USB/CDC (non-blocking writes, low priority) and keep all previous features.

  CHANGELOG (condensed):
  v73:
    - Restore missing v71 functions (pwmTask, ledTask, setPwmInputPin, reset_reason_to_cstr,
      write_mode_with_verify, load_mode_from_nvs, probeAnsiSupport).
    - RMT-based PSYN generator (non-blocking rmt_write_items, low-priority task).
    - Avoid UART freeze by ensuring PSYN task yields and backs off on driver resource shortage.
  v72:
    - Initial RMT-based PSYN implementation and integration.
  v71..v62:
    - Existing features kept: printer queue, NVS persistence, rainbow banner, median/hysteresis smoothing,
      OTA handling, typing suppression, POT/IBM sampling selection and commands, etc.

  NOTE:
    - This file intentionally restores the exact behavior and comments from v71 and integrates the PSYN changes.
    - I kept code structure straightforward: helper functions and task implementations appear before setup()
      so that task creation calls have their symbols available.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <TM1637Display.h>
#include "tusb.h"
#include <USB.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include <stdarg.h>
#include "esp_system.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "driver/rmt.h"
#include "soc/gpio_struct.h"

#include <math.h>
#include <string.h>

// ---------- Config & defaults ----------
static const unsigned long CDC_GRACE_MS = 2000UL;
static const int MAX_PENDING_MESSAGES = 128;
static const unsigned long ANSI_PROBE_TIMEOUT_MS = 200UL;
static const bool ASSUME_ANSI_IF_PROBE_FAIL = true;

static const int PRINTER_QUEUE_LEN_DEFAULT = 32;
static const unsigned long DBG_RATE_LIMIT_MS_DEFAULT = 650UL;

const char *NVS_KEY_DBG_RATE  = "dbg_rate_ms";
const char *NVS_KEY_PRTQ_LEN  = "prt_q_len";

void enable_usb_cdc(void) { tusb_init(); }

// ---------- Pins ----------
#define PWM_OUT_PIN  35
#define BUTTON_PIN   34
#ifndef LED_BUILTIN
#define LED_BUILTIN 13
#endif

static const int POT_PIN = 37;
static const int IBM_PIN = 36;
static volatile int pwmInPin = POT_PIN;

static const float MAX_POTV = 2.34f;
static const double POT_MAXV = (double)MAX_POTV;

// ---------- NVS keys ----------
const char *HYS_DELTA_KEY = "hys_delta";
const char *HYS_CONS_KEY  = "hys_cons";

// ---------- TACH / DISPLAY ----------
#define TACH_OUT_PIN 25
#define TM1637_CLK   16
#define TM1637_DIO   17
TM1637Display display(TM1637_CLK, TM1637_DIO);

// ---------- LEDC main ----------
#define LEDC_CHANNEL      0
#define LEDC_TIMER        0
#define LEDC_FREQ         21800
#define LEDC_RES_BITS     8
#define LEDC_MAX_DUTY     255

// ---------- RMT (PSYN) settings ----------
#define RMT_CHANNEL_TX    RMT_CHANNEL_0
static const int RMT_CLK_DIV = 8; // APB(80MHz)/8 = 10 MHz -> tick = 0.1 us
static const double PSYN_FREQ = 21500.0; // 21.5 kHz target

// ---------- Smoothing buffers ----------
const int DUTY_DISPLAY_FILTER_SIZE = 8;
int dutyDisplayBuffer[DUTY_DISPLAY_FILTER_SIZE];
int dutyDisplayIndex = 0;
int lastDisplayMedianDuty = -1;

int freqDisplayBuffer[DUTY_DISPLAY_FILTER_SIZE];
int freqDisplayIndex = 0;

const unsigned long DISPLAY_UPDATE_INTERVAL_US = 650000UL; // 650ms

// ---------- PWM sampling ----------
volatile unsigned long lastPwmPeriodUs = 0;
volatile unsigned long lastPwmHighUs = 0;
volatile int lastDutyRaw = -1;
volatile unsigned int lastFreqHz = 0;

// ---------- ISR temporaries ----------
volatile unsigned long isr_last_rise_us = 0;
volatile unsigned long isr_last_period_us = 0;
volatile unsigned long isr_last_high_us = 0;
volatile bool isr_new_data = false;

portMUX_TYPE isrMux = portMUX_INITIALIZER_UNLOCKED;

// ---------- Preferences / NVS ----------
Preferences prefs;
const char *NVS_NAMESPACE = "settings";
const char *NVS_KEY_MODE = "S=Mode";
volatile bool debug_mode_enabled = false;

// ---------- Hysteresis runtime config ----------
volatile int hysteresisImmediateDeltaPct = 3;
volatile int hysteresisRequiredConsistent = 2;

// ---------- New runtime parameters ----------
volatile unsigned long dbgRateLimitMs = DBG_RATE_LIMIT_MS_DEFAULT;
volatile int printerQueueLen = PRINTER_QUEUE_LEN_DEFAULT;

// ---------- Detail flag ----------
volatile bool detailEnabled = false;

// ---------- Typing / suppression flags ----------
volatile bool suppressOutputOnTyping = false;
volatile bool typingActive = false;
static String serialCmdBuffer = "";

// ---------- OTA flags ----------
volatile bool otaInProgress = false;
volatile bool reduceLoggingDuringOTA = false;
volatile bool otaStarted = false;

// ---------- PSYN synth state ----------
static bool psynActive = false;
static int psynFixedPct = -1; // -1 = coupled, >=0 fixed 5..99
static const int SYN_OUTPUT_PIN = 21; // GPIO21 synth output

// ---------- Button / long-press ----------
const unsigned long LONG_PRESS_THRESHOLD_MS = 2000UL;
const unsigned long PRE_REBOOT_DELAY_MS = 3000UL;

bool lastButtonState = HIGH;
unsigned long buttonDownTime = 0;
bool pending_reboot = false;
unsigned long rebootStartTime = 0;
bool next_mode = false;

// ---------- Tasks handles ----------
TaskHandle_t pwmTaskHandle = NULL;
TaskHandle_t displayTaskHandle = NULL;
TaskHandle_t ledTaskHandle = NULL;
TaskHandle_t otaTaskHandle = NULL;
TaskHandle_t tachTaskHandle = NULL;
TaskHandle_t rmtPsynTaskHandle = NULL;

// ---------- CDC / pending messages ----------
bool lastCdcConnected = false;
unsigned long lastCdcChangeTime = 0;
bool cdcGraceActive = false;

struct PendingEntry { bool used; char plain[256]; };
static PendingEntry pendingMessages[MAX_PENDING_MESSAGES];
static int pendingHead = 0;
static int pendingTail = 0;
static int pendingCount = 0;
SemaphoreHandle_t pendingMutex = NULL;
SemaphoreHandle_t displayMutex = NULL;
static bool bannerQueuedForGrace = false;

// ---------- ANSI colors ----------
bool ansiSupported = false;
static const char *ANSI_RESET = "\x1B[0m";
static const char *ANSI_RED = "\x1B[31m";
static const char *ANSI_YELLOW = "\x1B[33m";
static const char *ANSI_BOLD_YELLOW = "\x1B[93m";
static const char *ANSI_GREEN = "\x1B[32m";
static const char *ANSI_CYAN = "\x1B[36m";
static const char *ANSI_MAGENTA = "\x1B[35m";
static const char *ANSI_BLUE = "\x1B[34m";
static const char *ANSI_WHITE = "\x1B[37m";
static const char *ANSI_BOLD_WHITE = "\x1B[1;37m";
static const char *ANSI_BOLD_GREEN = "\x1B[1;32m";
static const char *ANSI_BOLD_CYAN = "\x1B[1;36m";
static const char *ANSI_BOLD_MAGENTA = "\x1B[1;35m";

// ---------- Banner ----------
static const char *RAW_BANNER = "=== IBM PS FAN CONTROL (c) 2025 by Purposeful Designs, Inc. === --- booting ---";
static String precoloredBanner;

// ---------- Prototypes ----------
static void enqueuePlain(const char *s);
static void flushPendingMessages();
static void logMsg(const char *fmt, ...);
static bool write_hysteresis_to_nvs(int delta, int cons);
static bool load_hysteresis_from_nvs();
static int computeMedianDuty();
static int computeMedianFreq();
static void setPwmInputPin(int pin);
static bool probeAnsiSupport(void);
static const char* reset_reason_to_cstr(esp_reset_reason_t r);
static bool write_mode_with_verify(bool mode, int retries, unsigned long retryDelayMs);
static bool load_mode_from_nvs(void);
static bool write_dbgparams_to_nvs(unsigned long dbg_ms, int qlen);
static bool load_dbgparams_from_nvs(void);
void startAPAndOTA_once(void);

// ---------- Printer queue and task ----------
struct PrintMsg { char txt[160]; };
static QueueHandle_t printerQueue = NULL;
static int printer_queue_len = PRINTER_QUEUE_LEN_DEFAULT;

static void printerEnqueue(const char *s) {
  if (!printerQueue) return;
  PrintMsg m;
  strncpy(m.txt, s, sizeof(m.txt) - 1);
  m.txt[sizeof(m.txt) - 1] = '\0';
  xQueueSend(printerQueue, &m, 0);
}

static void printerTask(void *pv) {
  (void)pv;
  PrintMsg m;
  for (;;) {
    if (xQueueReceive(printerQueue, &m, pdMS_TO_TICKS(200)) == pdTRUE) {
      if (tud_cdc_connected()) {
        const char *p = m.txt;
        size_t len = strlen(p);
        const size_t CHUNK = 64;
        size_t off = 0;
        while (off < len) {
          size_t n = min(CHUNK, len - off);
          Serial.write((const uint8_t*)(p + off), n);
          off += n;
          vTaskDelay(pdMS_TO_TICKS(2));
        }
        Serial.write('\r'); Serial.write('\n');
      }
    } else vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ---------- ISR ----------
void IRAM_ATTR pwm_isr() {
  unsigned long now = micros();
  int level = gpio_get_level((gpio_num_t)pwmInPin);
  portENTER_CRITICAL_ISR(&isrMux);
  if (level) {
    if (isr_last_rise_us != 0) isr_last_period_us = now - isr_last_rise_us;
    isr_last_rise_us = now;
  } else {
    if (isr_last_rise_us != 0) isr_last_high_us = now - isr_last_rise_us;
  }
  isr_new_data = true;
  portEXIT_CRITICAL_ISR(&isrMux);
}

// ---------- Colorize / banner helpers ----------
static void colorizeMessageForced(const char *in, char *out, size_t outlen) {
  if (outlen == 0) return;
  if (precoloredBanner.length() > 0 && strncmp(in, RAW_BANNER, strlen(RAW_BANNER)) == 0) {
    size_t copyLen = min((size_t)precoloredBanner.length(), outlen - 1);
    memcpy(out, precoloredBanner.c_str(), copyLen);
    out[copyLen] = '\0';
    return;
  }
  const char *color = ANSI_WHITE;
  if (strstr(in, "[ERROR]") || strstr(in, "[ERR]")) color = ANSI_RED;
  else if (strstr(in, "[WARN]") || strstr(in, "WARNING")) color = ANSI_YELLOW;
  else if (strstr(in, "[NVS]")) color = ANSI_BOLD_CYAN;
  else if (strstr(in, "[BOOT]")) color = ANSI_CYAN;
  else if (strstr(in, "[USB]")) color = ANSI_BOLD_MAGENTA;
  else if (strstr(in, "[PWM]")) color = ANSI_MAGENTA;
  else if (strstr(in, "[BTN]") || strstr(in, "[DISPLAY]")) color = ANSI_BOLD_YELLOW;
  else if (strstr(in, "[OTA]")) color = ANSI_GREEN;
  else if (strstr(in, "IBM PS FAN CONTROL") != NULL) color = ANSI_BOLD_GREEN;
  snprintf(out, outlen, "%s%s%s", color, in, ANSI_RESET);
}
static void colorizeMessage(const char *in, char *out, size_t outlen) {
  if (!ansiSupported) { strncpy(out, in, outlen - 1); out[outlen - 1] = '\0'; return; }
  colorizeMessageForced(in, out, outlen);
}
static void build_precolored_banner() {
  const char *bootTag = "--- booting ---";
  const char *marker  = "IBM PS FAN CONTROL";
  String sraw(RAW_BANNER);
  int posBoot = sraw.indexOf(bootTag);
  String left = (posBoot >= 0) ? sraw.substring(0, posBoot) : sraw;
  String right = (posBoot >= 0) ? sraw.substring(posBoot + strlen(bootTag)) : String("");
  int markerPos = left.indexOf(marker);
  String coloredPrefix;
  if (markerPos >= 0) {
    String before = left.substring(0, markerPos);
    String after = left.substring(markerPos + strlen(marker));
    coloredPrefix = String(ANSI_WHITE) + before + String(ANSI_BOLD_GREEN) + String(marker) + String(ANSI_WHITE) + after;
  } else coloredPrefix = String(ANSI_WHITE) + left;
  const char *cycle[] = { ANSI_RED, ANSI_BOLD_YELLOW, ANSI_GREEN, ANSI_CYAN, ANSI_MAGENTA, ANSI_BLUE };
  String coloredBoot;
  for (size_t i = 0; i < strlen(bootTag); ++i) {
    char ch = bootTag[i];
    if (ch == ' ') { coloredBoot += ' '; continue; }
    const char *col = cycle[i % 6];
    coloredBoot += String(col) + String(ch) + String(ANSI_WHITE);
  }
  precoloredBanner = coloredPrefix + coloredBoot + right + String(ANSI_RESET);
}

// ---------- Pending enqueue/flush ----------
static void enqueuePlain(const char *s) {
  if (!pendingMutex) return;
  if (xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  if (pendingCount >= MAX_PENDING_MESSAGES) { pendingHead = (pendingHead + 1) % MAX_PENDING_MESSAGES; pendingCount--; }
  int idx = pendingTail;
  strncpy(pendingMessages[idx].plain, s, sizeof(pendingMessages[idx].plain) - 1);
  pendingMessages[idx].plain[sizeof(pendingMessages[idx].plain) - 1] = '\0';
  pendingMessages[idx].used = true;
  pendingTail = (pendingTail + 1) % MAX_PENDING_MESSAGES;
  pendingCount++;
  xSemaphoreGive(pendingMutex);
}
static void flushPendingMessages() {
  if (!pendingMutex) return;
  if (xSemaphoreTake(pendingMutex, pdMS_TO_TICKS(200)) != pdTRUE) return;
  while (pendingCount > 0) {
    int idx = pendingHead;
    if (ansiSupported) { char out[768]; colorizeMessageForced(pendingMessages[idx].plain, out, sizeof(out)); Serial.println(out); }
    else Serial.println(pendingMessages[idx].plain);
    pendingMessages[idx].used = false;
    pendingHead = (pendingHead + 1) % MAX_PENDING_MESSAGES;
    pendingCount--;
  }
  xSemaphoreGive(pendingMutex);
}

// ---------- logging ----------
static void logMsg(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (suppressOutputOnTyping) { enqueuePlain(buf); return; }

  if (otaInProgress && reduceLoggingDuringOTA) {
    if (strstr(buf, "[OTA]") == NULL && strstr(buf, "[ERROR]") == NULL && strstr(buf, "[BOOT]") == NULL) return;
  }

  if (cdcGraceActive && (millis() - lastCdcChangeTime) < CDC_GRACE_MS) { enqueuePlain(buf); }
  else {
    if (cdcGraceActive) { cdcGraceActive = false; flushPendingMessages(); }
    if (ansiSupported) { char out[1024]; colorizeMessageForced(buf, out, sizeof(out)); Serial.println(out); }
    else Serial.println(buf);
  }
}

// ---------- NVS helpers ----------
static bool write_hysteresis_to_nvs(int delta, int cons) {
  for (int attempt = 1; attempt <= 4; ++attempt) {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putInt(HYS_DELTA_KEY, delta);
    prefs.putInt(HYS_CONS_KEY, cons);
    prefs.end();
    delay(80);
    prefs.begin(NVS_NAMESPACE, true);
    int rd1 = prefs.getInt(HYS_DELTA_KEY, -9999);
    int rd2 = prefs.getInt(HYS_CONS_KEY, -9999);
    prefs.end();
    if (rd1 == delta && rd2 == cons) { logMsg("[NVS] hysteresis persist ok delta=%d cons=%d", delta, cons); return true; }
    logMsg("[NVS] hysteresis persist verify mismatch attempt=%d got %d %d", attempt, rd1, rd2);
    delay(80);
  }
  logMsg("[NVS] hysteresis persist failed"); return false;
}
static bool load_hysteresis_from_nvs() {
  prefs.begin(NVS_NAMESPACE, true);
  int a = prefs.getInt(HYS_DELTA_KEY, -1);
  int b = prefs.getInt(HYS_CONS_KEY, -1);
  prefs.end();
  if (a >= 1 && b >= 1) { hysteresisImmediateDeltaPct = a; hysteresisRequiredConsistent = b; logMsg("[NVS] loaded hysteresis delta=%d cons=%d", a, b); return true; }
  logMsg("[NVS] no hysteresis found, using defaults delta=%d cons=%d", hysteresisImmediateDeltaPct, hysteresisRequiredConsistent);
  return false;
}

static bool write_dbgparams_to_nvs(unsigned long dbg_ms, int qlen) {
  for (int attempt = 1; attempt <= 4; ++attempt) {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putUInt(NVS_KEY_DBG_RATE, (uint32_t)dbg_ms);
    prefs.putInt(NVS_KEY_PRTQ_LEN, qlen);
    prefs.end();
    delay(80);
    prefs.begin(NVS_NAMESPACE, true);
    int rd1 = prefs.getInt(NVS_KEY_PRTQ_LEN, -9999);
    uint32_t rd2 = prefs.getUInt(NVS_KEY_DBG_RATE, 0);
    prefs.end();
    if (rd1 == qlen && rd2 == dbg_ms) { logMsg("[NVS] dbg params persist ok dbg_ms=%lu prtq=%d", dbg_ms, qlen); return true; }
    logMsg("[NVS] dbg params verify mismatch attempt=%d got dbg_ms=%u prtq=%d", attempt, (unsigned)rd2, rd1);
    delay(80);
  }
  logMsg("[NVS] dbg params persist failed"); return false;
}
static bool load_dbgparams_from_nvs(void) {
  prefs.begin(NVS_NAMESPACE, true);
  uint32_t dbg_ms = prefs.getUInt(NVS_KEY_DBG_RATE, 0);
  int qlen = prefs.getInt(NVS_KEY_PRTQ_LEN, -1);
  prefs.end();
  bool ok = false;
  if (dbg_ms >= 1) { dbgRateLimitMs = (unsigned long)dbg_ms; ok = true; }
  if (qlen >= 1 && qlen <= 1024) { printerQueueLen = qlen; ok = true; }
  if (ok) logMsg("[NVS] loaded dbgRateMs=%lu prtq=%d", dbgRateLimitMs, printerQueueLen);
  else logMsg("[NVS] no dbg params in NVS; using defaults dbg_ms=%lu prtq=%d", dbgRateLimitMs, printerQueueLen);
  return ok;
}

// ---------- median helpers ----------
static int computeMedianDuty() {
  int tmp[DUTY_DISPLAY_FILTER_SIZE];
  if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (int i = 0; i < DUTY_DISPLAY_FILTER_SIZE; ++i) tmp[i] = dutyDisplayBuffer[i];
    xSemaphoreGive(displayMutex);
  } else return (lastDisplayMedianDuty >= 0) ? lastDisplayMedianDuty : 0;
  for (int i = 1; i < DUTY_DISPLAY_FILTER_SIZE; ++i) {
    int key = tmp[i], j = i - 1;
    while (j >= 0 && tmp[j] > key) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = key;
  }
  int mid = DUTY_DISPLAY_FILTER_SIZE / 2;
  if (DUTY_DISPLAY_FILTER_SIZE & 1) return tmp[mid];
  return (tmp[mid - 1] + tmp[mid]) / 2;
}
static int computeMedianFreq() {
  int tmp[DUTY_DISPLAY_FILTER_SIZE];
  if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (int i = 0; i < DUTY_DISPLAY_FILTER_SIZE; ++i) tmp[i] = freqDisplayBuffer[i];
    xSemaphoreGive(displayMutex);
  } else return (int)lastFreqHz;
  for (int i = 1; i < DUTY_DISPLAY_FILTER_SIZE; ++i) {
    int key = tmp[i], j = i - 1;
    while (j >= 0 && tmp[j] > key) { tmp[j + 1] = tmp[j]; j--; }
    tmp[j + 1] = key;
  }
  int mid = DUTY_DISPLAY_FILTER_SIZE / 2;
  if (DUTY_DISPLAY_FILTER_SIZE & 1) return tmp[mid];
  return (tmp[mid - 1] + tmp[mid]) / 2;
}

// ---------- Probe ANSI ----------
static bool probeAnsiSupport(void) {
  while (Serial.available()) Serial.read();
  Serial.print("\x1B[6n");
  unsigned long start = millis();
  bool sawEscape = false, sawBracket = false, sawR = false;
  while (millis() - start < ANSI_PROBE_TIMEOUT_MS) {
    while (Serial.available()) {
      int c = Serial.read();
      if (c < 0) continue;
      if (!sawEscape && c == 0x1B) sawEscape = true;
      else if (sawEscape && !sawBracket && c == '[') sawBracket = true;
      else if (sawBracket && c == 'R') sawR = true;
    }
    if (sawR) break;
    delay(2);
  }
  while (Serial.available()) Serial.read();
  return sawR;
}

// ---------- load/write mode helpers ----------
static bool write_mode_with_verify(bool mode, int retries, unsigned long retryDelayMs) {
  for (int attempt = 1; attempt <= retries; ++attempt) {
    prefs.begin(NVS_NAMESPACE, false);
    prefs.putInt(NVS_KEY_MODE, mode ? 1 : 0);
    prefs.end();
    delay(80);
    prefs.begin(NVS_NAMESPACE, true);
    int readback = prefs.getInt(NVS_KEY_MODE, -1);
    prefs.end();
    if (readback == (mode ? 1 : 0)) {
      logMsg("[NVS] Verified mode write %d", mode ? 1 : 0);
      return true;
    }
    logMsg("[NVS] Mode write verify mismatch attempt=%d readback=%d", attempt, readback);
    delay(retryDelayMs);
  }
  logMsg("[NVS] Mode write failed after retries");
  return false;
}
static bool load_mode_from_nvs(void) {
  prefs.begin(NVS_NAMESPACE, true);
  int val = prefs.getInt(NVS_KEY_MODE, -1);
  prefs.end();
  if (val < 0) {
    logMsg("[NVS] Mode key missing; default STABLE (0)");
    return false;
  }
  logMsg("[NVS] Loaded mode=%d", val);
  return (val != 0);
}

// ---------- reset reason helper ----------
static const char* reset_reason_to_cstr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_UNKNOWN:   return "UNKNOWN";
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    default: return "UNMAPPED";
  }
}

// ---------- set PWM input pin ----------
static void setPwmInputPin(int pin) {
  if (pin == pwmInPin) return;
  int prev = pwmInPin;
  if (digitalPinToInterrupt(prev) != NOT_AN_INTERRUPT) detachInterrupt(digitalPinToInterrupt(prev));
  pwmInPin = pin;
  pinMode(pwmInPin, INPUT);
  portENTER_CRITICAL(&isrMux);
  isr_last_rise_us = 0; isr_last_period_us = 0; isr_last_high_us = 0; isr_new_data = false;
  lastPwmPeriodUs = lastPwmHighUs = lastFreqHz = 0; lastDutyRaw = -1;
  portEXIT_CRITICAL(&isrMux);
  attachInterrupt(digitalPinToInterrupt(pwmInPin), pwm_isr, CHANGE);
  logMsg("[PWM] sampling changed to GPIO%d", pwmInPin);
}

// ---------- pwmTask (sampling) ----------
void pwmTask(void *pv) {
  (void)pv;
  logMsg("[PWM] sampling task start on GPIO%d", pwmInPin);
  for (;;) {
    bool newData = false;
    portENTER_CRITICAL(&isrMux);
    newData = isr_new_data;
    if (newData) {
      if (isr_last_period_us != 0) lastPwmPeriodUs = isr_last_period_us;
      if (isr_last_high_us != 0) lastPwmHighUs = isr_last_high_us;
      isr_new_data = false;
    }
    portEXIT_CRITICAL(&isrMux);

    if (newData) {
      unsigned long period = lastPwmPeriodUs;
      unsigned long high = lastPwmHighUs;
      int duty = -1;
      unsigned int freq = 0;
      if (period > 0) {
        duty = (int)((LEDC_MAX_DUTY * high) / period);
        duty = constrain(duty, 0, LEDC_MAX_DUTY);
        freq = (unsigned int)(1000000UL / period);
      } else duty = -1;

      if (duty >= 0) {
        lastDutyRaw = duty;
        lastFreqHz = freq;

        if (xSemaphoreTake(displayMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
          dutyDisplayBuffer[dutyDisplayIndex] = duty;
          dutyDisplayIndex = (dutyDisplayIndex + 1) % DUTY_DISPLAY_FILTER_SIZE;
          freqDisplayBuffer[freqDisplayIndex] = (int)freq;
          freqDisplayIndex = (freqDisplayIndex + 1) % DUTY_DISPLAY_FILTER_SIZE;
          xSemaphoreGive(displayMutex);
        }

        // keep main LEDC in sync
        ledcWrite(LEDC_CHANNEL, duty);
        // RMT PSYN task will use lastDutyRaw when coupled
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// ---------- ledTask (from v71) ----------
void ledTask(void *pv) {
  (void)pv;
  pinMode(LED_BUILTIN, OUTPUT);
  for (;;) {
    if (!debug_mode_enabled) {
      // STABLE: ON 1s, OFF 3s
      digitalWrite(LED_BUILTIN, HIGH);
      vTaskDelay(pdMS_TO_TICKS(1000));
      digitalWrite(LED_BUILTIN, LOW);
      vTaskDelay(pdMS_TO_TICKS(3000));
    } else {
      // DEBUG: two quick blinks (300ms each with 300ms gap), then OFF 5s
      digitalWrite(LED_BUILTIN, HIGH);
      vTaskDelay(pdMS_TO_TICKS(300));
      digitalWrite(LED_BUILTIN, LOW);
      vTaskDelay(pdMS_TO_TICKS(300));
      digitalWrite(LED_BUILTIN, HIGH);
      vTaskDelay(pdMS_TO_TICKS(300));
      digitalWrite(LED_BUILTIN, LOW);
      vTaskDelay(pdMS_TO_TICKS(5000));
    }
  }
}

// ---------- Tach synth ----------
void tachSynthTask(void *pv) {
  (void)pv;
  pinMode(TACH_OUT_PIN, OUTPUT);
  digitalWrite(TACH_OUT_PIN, LOW);
  logMsg("[TACH] running");
  for (;;) {
    unsigned long period;
    portENTER_CRITICAL(&isrMux);
    period = lastPwmPeriodUs;
    portEXIT_CRITICAL(&isrMux);
    if (period == 0) {
      int d = lastDutyRaw; if (d < 0) d = 0;
      unsigned int f = 5 + (unsigned int)((195UL * (unsigned)d) / 255UL);
      unsigned long half = (f > 0) ? (500000UL / f) : 100000UL;
      digitalWrite(TACH_OUT_PIN, HIGH); ets_delay_us(half);
      digitalWrite(TACH_OUT_PIN, LOW); ets_delay_us(half);
    } else {
      unsigned long half = period / 2;
      if (half < 50) half = 50;
      digitalWrite(TACH_OUT_PIN, HIGH); ets_delay_us(half);
      digitalWrite(TACH_OUT_PIN, LOW); ets_delay_us(half);
    }
    if (!otaInProgress) break;
  }
  digitalWrite(TACH_OUT_PIN, LOW);
  vTaskDelete(NULL);
}

// ---------- RMT PSYN TX task (non-blocking, low-priority) ----------
static void rmtPsynTask(void *pv) {
  (void)pv;

  const double apb_hz = 80000000.0;
  const double rmt_tick_hz = apb_hz / (double)RMT_CLK_DIV;
  const double period_ticks_d = rmt_tick_hz / PSYN_FREQ; // fractional ticks per period

  double err_accum = 0.0;

  rmt_item32_t item;
  memset(&item, 0, sizeof(item));

  for (;;) {
    if (!psynActive) {
      // ensure idle low when PSYN not active
      gpio_set_level((gpio_num_t)SYN_OUTPUT_PIN, 0);
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }

    int duty_pct;
    if (psynFixedPct >= 0) {
      duty_pct = psynFixedPct;
    } else {
      int localLast = lastDutyRaw;
      if (localLast < 0) localLast = 0;
      duty_pct = (int)round((100.0 * (double)localLast) / (double)LEDC_MAX_DUTY);
      duty_pct = constrain(duty_pct, 0, 100);
    }

    // ideal high ticks (fractional)
    double ideal_high_ticks = period_ticks_d * ((double)duty_pct / 100.0);

    // take rounding with accumulated error to distribute fractional parts
    double high_with_err = ideal_high_ticks + err_accum;
    int high_ticks = (int)floor(high_with_err + 0.5); // round nearest
    err_accum = high_with_err - (double)high_ticks;

    if (high_ticks < 0) high_ticks = 0;
    if (high_ticks > 0x7fff) high_ticks = 0x7fff;

    int low_ticks = (int)round(period_ticks_d - (double)high_ticks);
    if (low_ticks < 0) low_ticks = 0;
    if (low_ticks > 0x7fff) low_ticks = 0x7fff;

    if (high_ticks == 0 && duty_pct > 0) high_ticks = 1;
    if (low_ticks == 0 && duty_pct < 100) low_ticks = 1;

    item.level0 = 1;
    item.duration0 = (uint32_t)high_ticks;
    item.level1 = 0;
    item.duration1 = (uint32_t)low_ticks;

    // Use non-blocking write (wait=false). If driver queue is full, back off a bit.
    esp_err_t res = rmt_write_items(RMT_CHANNEL_TX, &item, 1, false);
    if (res == ESP_OK) {
      // queued; yield to let other tasks run
      taskYIELD();
    } else if (res == ESP_ERR_INVALID_STATE || res == ESP_ERR_NO_MEM) {
      // temporary resource problem, sleep a tiny bit so serial/USB tasks get CPU
      vTaskDelay(pdMS_TO_TICKS(1));
    } else {
      // unexpected error: log and continue with small backoff
      logMsg("[PSYN] rmt_write_items error %d", res);
      vTaskDelay(pdMS_TO_TICKS(2));
    }
  }
}

// ---------- Display task (keeps display and detail DBG) ----------
void startDisplayTaskIfNotRunning() {
  if (!debug_mode_enabled) return;
  if (displayTaskHandle != NULL) return;

  display.setBrightness(0x04, true);
  display.showNumberDec(0, true, 2, 2);

  xTaskCreatePinnedToCore([](void *pv) {
    (void)pv;
    uint8_t blankSegs[2] = {0x00,0x00};
    int lastShownPct = 0, candidatePct = -1, candidateCount = 0;
    unsigned long lastDbgEnqueueMs = 0;

    logMsg("[DISPLAY] Display task started (v73).");
    unsigned long lastUpdateUs = 0;
    for (;;) {
      unsigned long now = micros();
      if (now - lastUpdateUs >= DISPLAY_UPDATE_INTERVAL_US) {
        lastUpdateUs = now;
        int medianDuty = computeMedianDuty();
        if (medianDuty != lastDisplayMedianDuty) lastDisplayMedianDuty = medianDuty;

        int pct = 0;
        if (medianDuty <= 0) pct = 0;
        else {
          double pf = (100.0 * (double)medianDuty) / (double)LEDC_MAX_DUTY;
          int ip = (int)round(pf);
          if (ip < 1) ip = 1;
          if (ip > 99) ip = 99;
          pct = ip;
        }

        int IMMEDIATE_DELTA_PCT = hysteresisImmediateDeltaPct;
        int REQUIRED_CONSISTENT = hysteresisRequiredConsistent;

        if (pct == lastShownPct) {
          candidatePct = -1; candidateCount = 0;
        } else {
          int diff = abs(pct - lastShownPct);
          if (diff <= IMMEDIATE_DELTA_PCT) {
            lastShownPct = pct; candidatePct = -1; candidateCount = 0;
          } else {
            if (candidatePct != pct) { candidatePct = pct; candidateCount = 1; }
            else candidateCount++;
            if (candidateCount >= REQUIRED_CONSISTENT) { lastShownPct = candidatePct; candidatePct = -1; candidateCount = 0; }
          }
        }

        display.showNumberDec(lastShownPct, true, 2, 2);
        display.setSegments(blankSegs, 2, 0);

        if (detailEnabled && !suppressOutputOnTyping) {
          unsigned long nowMs = millis();
          if (nowMs - lastDbgEnqueueMs >= dbgRateLimitMs) {
            int medianFreqHz = computeMedianFreq();
            bool freq_ok = (medianFreqHz > 0 && medianFreqHz < 1000000);
            bool duty_ok = (medianDuty >= 0 && medianDuty <= LEDC_MAX_DUTY);
            char dbg[160];
            if (freq_ok && duty_ok) {
              double freqKHz = ((double)medianFreqHz) / 1000.0;
              double estimatedV = 0.0;
              if (lastShownPct > 0) estimatedV = ((double)lastShownPct / 99.0) * POT_MAXV;
              snprintf(dbg, sizeof(dbg), "[DISPLAY-DBG] pct=%02d freq=%.1fkHz Vest=%.2fV raw=%d",
                       lastShownPct, freqKHz, estimatedV, medianDuty);
            } else {
              snprintf(dbg, sizeof(dbg), "[DISPLAY-DBG] pct=%02d freq=--.-kHz Vest=--.--V raw=%d (suppressed)", lastShownPct, medianDuty);
            }
            printerEnqueue(dbg);
            lastDbgEnqueueMs = nowMs;
          }
        }
      }
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }, "Display", 3072, NULL, 2, &displayTaskHandle, tskNO_AFFINITY);
}

// ---------- OTA ----------
void startAPAndOTA_once() {
  static bool started = false;
  if (started) return;
  started = true;
  logMsg("[OTA] init WiFi/OTA...");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin("IBM_PSFAN", "gepa12,12,12");

  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    reduceLoggingDuringOTA = true;
    logMsg("[OTA] Start - reduced logging");
    int savedDuty = lastDutyRaw; if (savedDuty < 0) savedDuty = 0;
    ledcWrite(LEDC_CHANNEL, savedDuty);
    if (tachTaskHandle == NULL) {
      xTaskCreatePinnedToCore(tachSynthTask, "TACH", 2048, NULL, configMAX_PRIORITIES - 4, &tachTaskHandle, tskNO_AFFINITY);
    }
  });

  ArduinoOTA.onEnd([]() {
    otaInProgress = false;
    reduceLoggingDuringOTA = false;
    logMsg("[OTA] End - restored logging");
    if (tachTaskHandle != NULL) { vTaskDelete(tachTaskHandle); tachTaskHandle = NULL; }
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    char buf[80];
    snprintf(buf, sizeof(buf), "[OTA] %u%%", (progress * 100) / total);
    logMsg("%s", buf);
  });

  ArduinoOTA.onError([](ota_error_t error) {
    otaInProgress = false;
    reduceLoggingDuringOTA = false;
    logMsg("[OTA] Error %u", (unsigned)error);
    if (tachTaskHandle != NULL) { vTaskDelete(tachTaskHandle); tachTaskHandle = NULL; }
  });

  ArduinoOTA.begin();
  otaStarted = true;
  logMsg("[OTA] ArduinoOTA initialized");
}

// ---------- Setup ----------
void setup() {
  delay(50);
  pendingMutex = xSemaphoreCreateMutex();
  displayMutex = xSemaphoreCreateMutex();

  enable_usb_cdc();
  USB.begin();
  delay(20);

  Serial.begin(115200);
  delay(10);

  lastCdcConnected = tud_cdc_connected();
  lastCdcChangeTime = millis();
  cdcGraceActive = true;
  pendingCount = 0;

  // Build rainbow precolored banner
  build_precolored_banner();

  ansiSupported = lastCdcConnected ? probeAnsiSupport() || ASSUME_ANSI_IF_PROBE_FAIL : false;

  enqueuePlain(RAW_BANNER);

  for (int i = 0; i < DUTY_DISPLAY_FILTER_SIZE; ++i) {
    dutyDisplayBuffer[i] = 0;
    freqDisplayBuffer[i] = 0;
  }
  dutyDisplayIndex = freqDisplayIndex = 0;
  lastDutyRaw = -1;
  lastPwmPeriodUs = lastPwmHighUs = lastFreqHz = 0;

  pinMode(pwmInPin, INPUT);
  pinMode(PWM_OUT_PIN, OUTPUT);
  pinMode(TACH_OUT_PIN, OUTPUT);
  digitalWrite(TACH_OUT_PIN, LOW);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_BUILTIN, OUTPUT);

  // Setup RMT for PSYN
  gpio_set_direction((gpio_num_t)SYN_OUTPUT_PIN, GPIO_MODE_OUTPUT);
  gpio_pullup_en((gpio_num_t)SYN_OUTPUT_PIN);

  rmt_config_t rmt_tx;
  rmt_tx.channel = RMT_CHANNEL_TX;
  rmt_tx.gpio_num = (gpio_num_t)SYN_OUTPUT_PIN;
  rmt_tx.clk_div = RMT_CLK_DIV;
  rmt_tx.mem_block_num = 1;
  rmt_tx.rmt_mode = RMT_MODE_TX;
  rmt_tx.tx_config.loop_en = false;
  rmt_tx.tx_config.carrier_en = false;
  rmt_tx.tx_config.idle_output_en = true;
  rmt_tx.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
  rmt_tx.tx_config.carrier_level = RMT_CARRIER_LEVEL_LOW;
  rmt_config(&rmt_tx);
  rmt_driver_install(rmt_tx.channel, 0, 0);

  debug_mode_enabled = load_mode_from_nvs();
  load_hysteresis_from_nvs();
  load_dbgparams_from_nvs();

  // create printer queue and task using persisted print queue length
  printer_queue_len = printerQueueLen;
  printerQueue = xQueueCreate((UBaseType_t)printer_queue_len, sizeof(PrintMsg));
  if (printerQueue) {
    xTaskCreate(printerTask, "Printer", 2048, NULL, 1, NULL);
    logMsg("[PRINTER] printer task started with queue length %d", printer_queue_len);
  } else {
    logMsg("[PRINTER] Failed to create printer queue/task with len=%d; using direct prints", printer_queue_len);
  }

  // LEDC main output
  ledcSetup(LEDC_CHANNEL, LEDC_FREQ, LEDC_RES_BITS);
  ledcAttachPin(PWM_OUT_PIN, LEDC_CHANNEL);
  ledcWrite(LEDC_CHANNEL, 0);

  // create RMT PSYN task at low priority (1) to avoid starving USB/Serial/loop()
  if (rmtPsynTaskHandle == NULL) {
    xTaskCreatePinnedToCore(rmtPsynTask, "PSYN_RMT", 2048, NULL, 1, &rmtPsynTaskHandle, tskNO_AFFINITY);
  }

  // Attach interrupt sampling and create tasks
  attachInterrupt(digitalPinToInterrupt(pwmInPin), pwm_isr, CHANGE);
  if (pwmTaskHandle == NULL) {
    xTaskCreatePinnedToCore(pwmTask, "PWM", 4096, NULL, configMAX_PRIORITIES - 2, &pwmTaskHandle, tskNO_AFFINITY);
  }
  if (ledTaskHandle == NULL) {
    xTaskCreatePinnedToCore(ledTask, "LED", 1536, NULL, 2, &ledTaskHandle, tskNO_AFFINITY);
  }

  if (debug_mode_enabled) {
    if (otaTaskHandle == NULL) {
      xTaskCreatePinnedToCore([](void*pv){
        (void)pv;
        for (;;) {
          if (otaInProgress) {
            ArduinoOTA.handle();
            vTaskDelay(pdMS_TO_TICKS(20));
          } else vTaskDelay(pdMS_TO_TICKS(250));
        }
      }, "OTA", 3072, NULL, configMAX_PRIORITIES - 3, &otaTaskHandle, tskNO_AFFINITY);
    }
    startDisplayTaskIfNotRunning();
    startAPAndOTA_once();
  }

  logMsg("Booted in %s mode", debug_mode_enabled ? "DEBUG" : "STABLE");
  esp_reset_reason_t rr = esp_reset_reason();
  logMsg("[BOOT] reset_reason=%s, free_heap=%u", reset_reason_to_cstr(rr), (unsigned)esp_get_free_heap_size());

  delay(50);
}

// ---------- Loop ----------
void loop() {
  if (Serial.available()) {
    while (Serial.available()) {
      int c = Serial.read();
      if (c < 0) continue;

      if (!typingActive) {
        typingActive = true;
        suppressOutputOnTyping = true;
        if (tud_cdc_connected()) { Serial.write('>'); Serial.write(' '); }
      }

      if (c == '\r') {
        if (tud_cdc_connected()) Serial.write('\r');
      } else if (c == '\n') {
        if (tud_cdc_connected()) Serial.write('\n');
        String cmd = serialCmdBuffer;
        cmd.trim();
        cmd.toUpperCase();

        typingActive = false;
        suppressOutputOnTyping = false;

        // Existing commands...
        if (cmd == "POT") {
          setPwmInputPin(POT_PIN);
          logMsg("[SERIAL] POT selected");
        } else if (cmd == "IBM") {
          setPwmInputPin(IBM_PIN);
          logMsg("[SERIAL] IBM selected");
        } else if (cmd == "DETAIL") {
          detailEnabled = true;
          logMsg("[DISPLAY] DETAIL ON");
        } else if (cmd == "NODETAIL") {
          detailEnabled = false;
          logMsg("[DISPLAY] DETAIL OFF");
        } else if (cmd.startsWith("SET DELTA ")) {
          String arg = cmd.substring(strlen("SET DELTA "));
          int v = arg.toInt();
          if (v >= 0 && v <= 100) {
            hysteresisImmediateDeltaPct = v;
            bool ok = write_hysteresis_to_nvs(hysteresisImmediateDeltaPct, hysteresisRequiredConsistent);
            logMsg("[CFG] SET DELTA -> %d (persist=%s)", hysteresisImmediateDeltaPct, ok ? "ok":"fail");
          } else logMsg("[CFG] Invalid SET DELTA argument: %s", arg.c_str());
        } else if (cmd.startsWith("SET CONSISTENT ")) {
          String arg = cmd.substring(strlen("SET CONSISTENT "));
          int v = arg.toInt();
          if (v >= 1 && v <= 10) {
            hysteresisRequiredConsistent = v;
            bool ok = write_hysteresis_to_nvs(hysteresisImmediateDeltaPct, hysteresisRequiredConsistent);
            logMsg("[CFG] SET CONSISTENT -> %d (persist=%s)", hysteresisRequiredConsistent, ok ? "ok":"fail");
          } else logMsg("[CFG] Invalid SET CONSISTENT argument: %s", arg.c_str());
        } else if (cmd.startsWith("SET DBGRATE ")) {
          String arg = cmd.substring(strlen("SET DBGRATE "));
          int v = arg.toInt();
          if (v >= 50 && v <= 10000) {
            dbgRateLimitMs = (unsigned long)v;
            bool ok = write_dbgparams_to_nvs(dbgRateLimitMs, printerQueueLen);
            logMsg("[CFG] SET DBGRATE -> %lu ms (persist=%s)", dbgRateLimitMs, ok ? "ok":"fail");
          } else logMsg("[CFG] Invalid SET DBGRATE argument: %s", arg.c_str());
        } else if (cmd.startsWith("SET PRTQ ")) {
          String arg = cmd.substring(strlen("SET PRTQ "));
          int v = arg.toInt();
          if (v >= 4 && v <= 512) {
            printerQueueLen = v;
            bool ok = write_dbgparams_to_nvs(dbgRateLimitMs, printerQueueLen);
            logMsg("[CFG] SET PRTQ -> %d (persist=%s) (reboot to apply)", printerQueueLen, ok ? "ok":"fail");
          } else logMsg("[CFG] Invalid SET PRTQ argument: %s", arg.c_str());
        } else if (cmd == "SHOWCFG") {
          logMsg("[CFG] hysteresis delta=%d consistent=%d detail=%s dbgRateMs=%lu prtq=%d",
                 hysteresisImmediateDeltaPct, hysteresisRequiredConsistent, detailEnabled ? "ON":"OFF", dbgRateLimitMs, printerQueueLen);
        }
        // PSYN handling: "PSYN" [n] or "PSYN OFF"
        else if (cmd.startsWith("PSYN")) {
          String tail = "";
          if (cmd.length() > 4) { tail = cmd.substring(4); tail.trim(); }
          if (tail.length() == 0) {
            psynActive = true;
            psynFixedPct = -1;
            const char *which = (pwmInPin == POT_PIN) ? "POT" : "IBM";
            logMsg("[SERIAL] PWM SYNTH coupled to detected %s duty", which);
            logMsg("[SERIAL] Remember you can set a \"fake\", fixed PWM duty using the PSYN n command (n=5 to 99)");
            if (lastDutyRaw >= 0) {
              // RMT task will pick it up on next cycle
            }
          } else {
            if (tail == "OFF") {
              psynActive = false;
              psynFixedPct = -1;
              gpio_set_level((gpio_num_t)SYN_OUTPUT_PIN, 0);
              logMsg("[SERIAL] PWM SYNTH stopped");
            } else {
              int v = tail.toInt();
              if (v >= 5 && v <= 99) {
                psynActive = true;
                psynFixedPct = v;
                logMsg("[SERIAL] PWM SYNTH at duty = %d%%", v);
              } else {
                logMsg("[SERIAL] PSYN invalid argument (use PSYN n  where n=5..99 or PSYN OFF)");
              }
            }
          }
        }
        else if (cmd.length() > 0) {
          logMsg("[SERIAL] Unrecognized: %s", cmd.c_str());
        }

        flushPendingMessages();
        serialCmdBuffer = "";
      } else if (c == 0x7F || c == 0x08) {
        if (serialCmdBuffer.length() > 0) {
          serialCmdBuffer.remove(serialCmdBuffer.length() - 1);
          if (tud_cdc_connected()) { Serial.write(8); Serial.write(' '); Serial.write(8); }
        }
      } else {
        if (tud_cdc_connected()) Serial.write((uint8_t)c);
        serialCmdBuffer += (char)c;
        if (serialCmdBuffer.length() > 128) serialCmdBuffer = serialCmdBuffer.substring(serialCmdBuffer.length() - 128);
      }
    }
  }

  // Button handling
  bool btnState = digitalRead(BUTTON_PIN);
  unsigned long nowMs = millis();
  if (btnState == LOW && lastButtonState == HIGH) {
    buttonDownTime = nowMs;
  } else if (btnState == HIGH && lastButtonState == LOW) {
    unsigned long pressDuration = nowMs - buttonDownTime;
    logMsg("[BTN] Released after %lums", pressDuration);
    if (pressDuration >= LONG_PRESS_THRESHOLD_MS) {
      pending_reboot = true;
      next_mode = !debug_mode_enabled;
      rebootStartTime = nowMs;
      logMsg("[BTN] long press, will reboot into %s after %lums", next_mode ? "DEBUG":"STABLE", PRE_REBOOT_DELAY_MS);
    } else logMsg("[BTN] Short press ignored.");
  }
  lastButtonState = btnState;

  // CDC connect/disconnect
  bool cdcConnected = tud_cdc_connected();
  if (cdcConnected != lastCdcConnected) {
    lastCdcChangeTime = millis();
    cdcGraceActive = true;
    bannerQueuedForGrace = false;
    enqueuePlain(RAW_BANNER);
    bannerQueuedForGrace = true;

    if (cdcConnected) {
      delay(30);
      ansiSupported = probeAnsiSupport() || ASSUME_ANSI_IF_PROBE_FAIL;
      char info[256];
      snprintf(info, sizeof(info), "[USB] CDC connected - MODE: %s. Sampling pin: GPIO%d", debug_mode_enabled ? "DEBUG":"STABLE", pwmInPin);
      enqueuePlain(info);
      if (debug_mode_enabled) { startDisplayTaskIfNotRunning(); startAPAndOTA_once(); }
    } else {
      char info[192];
      snprintf(info, sizeof(info), "[USB] CDC disconnected - Sampling pin: GPIO%d", pwmInPin);
      enqueuePlain(info);
      if (displayTaskHandle != NULL) {
        vTaskDelete(displayTaskHandle);
        displayTaskHandle = NULL;
      }
    }
    lastCdcConnected = cdcConnected;
  }

  if (cdcGraceActive && (millis() - lastCdcChangeTime) >= CDC_GRACE_MS) {
    cdcGraceActive = false;
    flushPendingMessages();
  }

  if (pending_reboot) {
    if (btnState == LOW && (millis() - rebootStartTime) < PRE_REBOOT_DELAY_MS) {
      pending_reboot = false;
      logMsg("[BTN] Pending reboot canceled by user during grace period.");
    } else if ((millis() - rebootStartTime) >= PRE_REBOOT_DELAY_MS) {
      logMsg("[BOOT] Grace elapsed. Saving next_mode=%d and restarting now.", next_mode ? 1 : 0);
      bool ok = write_mode_with_verify(next_mode, 4, 200);
      if (!ok) logMsg("[BOOT] WARNING: NVS write verification failed; proceeding to restart.");
      delay(250);
      logMsg("[BOOT] Restarting...");
      delay(50);
      ESP.restart();
    }
  }

  if (debug_mode_enabled && otaStarted && lastCdcConnected) {
    ArduinoOTA.handle();
  }

  delay(1);
}