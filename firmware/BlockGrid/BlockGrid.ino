// ---------------------------------------------------------------------------
// BlockGrid v1.0
//
// A desk-size Bitcoin dashboard for an ESP32 driving a Waveshare
// RGB-Matrix-P2.5-96x48-F (96x48 HUB75) LED panel: the current block height,
// the price in USD, EUR or JPY with its 24-hour change, and sats per $1, €1
// or ¥100. Wi-Fi is set up through a captive portal; everything else is set
// on a web control page or with two optional buttons.
//
// Before uploading:
//  - set PANEL_VERSION and HAS_BUTTONS below
//  - keep build_opt.h next to this file, and restart the Arduino IDE once
//    after adding it (it sets the panel library's colour depth)
//
// https://github.com/n0cturnality/BlockGrid
// MIT License, (c) 2026 n0cturnality. See LICENSE.
// ---------------------------------------------------------------------------

#define BLOCKGRID_VERSION "1.0"

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
// Built-in synchronous web server (part of the ESP32 board package). Used
// instead of ESPAsyncWebServer/AsyncTCP: the async one runs its own task
// with a 16 KB stack, and keeping that alive while on Wi-Fi left too little
// free memory for the HTTPS price fetches (they failed with -1). This one
// needs no task of its own — loop() serves requests between its other work.
#include <WebServer.h>
#include <DNSServer.h>   // setup mode's captive portal (see handleNotFound())
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <atomic>
#include <esp_timer.h>
#include <stdint.h>
#include <math.h>
#include <esp_heap_caps.h> // for heap_caps_get_largest_free_block() — total free bytes alone doesn't show fragmentation; this does

// ---------- Physical buttons: fitted or not ----------
// 1 = the two push buttons are fitted (brightness / brand / currency /
//     hold-both-to-reboot, as well as the web control page).
// 0 = a display built without buttons: everything is done from the web
//     control page (http://blockgrid-xxxx.local). The button pins are left
//     alone and never read, so nothing needs to be connected to them.
#define HAS_BUTTONS 1

// ---------- Pin definitions ----------
#define R1_PIN_DEFAULT  25
#define G1_PIN_DEFAULT  26
#define B1_PIN_DEFAULT  27
#define R2_PIN_DEFAULT  14
#define G2_PIN_DEFAULT  12
#define B2_PIN_DEFAULT  13

#define A_PIN_DEFAULT   23
#define B_PIN_DEFAULT   19
#define C_PIN_DEFAULT   5
#define D_PIN_DEFAULT   17
#define E_PIN_DEFAULT   33

#define LAT_PIN_DEFAULT 4
#define OE_PIN_DEFAULT  15
#define CLK_PIN_DEFAULT 16

// Two independent buttons, each doing double duty: short press adjusts
// brightness (top button = up, bottom button = down), long press cycles
// branding (top) or currency (bottom). Both wired to GND with the
// internal pull-up enabled (INPUT_PULLUP) — no external resistors needed.
// Idle = HIGH, pressed = LOW.
#define BUTTON_BRAND_PIN    32
#define BUTTON_CURRENCY_PIN 18   // not GPIO33: that pin is the HUB75 panel's E address line (E_PIN_DEFAULT)
#define BUTTON_DEBOUNCE_MS 50UL

// Short press = brightness step (fires on release). Long press = the
// existing brand/currency cycling (fires the instant the hold crosses this
// threshold, not on release, so it feels responsive rather than delayed).
#define LONG_PRESS_THRESHOLD_MS 600UL
// Holding BOTH buttons together this long triggers a manual reboot —
// deliberately much longer than LONG_PRESS_THRESHOLD_MS so it reads as a
// clearly distinct, deliberate gesture rather than an extension of the
// normal single-button long press.
#define REBOOT_HOLD_MS 3000UL
// Brightness runs 1–255 (0 is "display off", a separate switch). The
// buttons step through BRIGHTNESS_LEVELS: small steps at the dim end, where
// the eye notices each one most, about a third brighter each step at the
// bright end. The web page's −/+ use the same levels, and its slider the
// same kind of curve.
// Very low levels: the panel library dims by switching the LEDs on for a
// few clock ticks per refresh, so near the bottom there are few ticks to
// share out and mixed colours (orange, the rain's dim greens) can shift.
// Pure red, green, blue and white stay true. Raise BRIGHTNESS_MIN if the
// lowest levels don't look right on your panel.
#define BRIGHTNESS_MIN  1
#define BRIGHTNESS_MAX  255
const uint8_t BRIGHTNESS_LEVELS[] = { 1, 2, 3, 4, 5, 6, 8, 10, 13, 17, 22, 28, 36, 46, 60, 77, 100, 128, 165, 210, 255 };
#define NUM_BRIGHTNESS_LEVELS (sizeof(BRIGHTNESS_LEVELS) / sizeof(BRIGHTNESS_LEVELS[0]))

// ---------- Panel config ----------
#define PANEL_RES_X 96
#define PANEL_RES_Y 48
#define PANEL_CHAIN 1

// Which hardware version of the Waveshare RGB-Matrix-P2.5-96x48-F is
// connected. Check the silkscreen on the back of the panel:
//   "...-24S-A2.1"  -> 1  (original version: 3-to-8 row decoders on A-E)
//   "...-24S-V2.1"  -> 1  (the same original version, printed V2.1 on some)
//   "...-24S-A1"    -> 2  (newer version: SM5368 row drivers)
// V2 needs different row driving (per Waveshare's own V2 instructions) and
// has its red and blue data lines the other way round; the wiring is the
// same for both. See the panel config block in setup().
#define PANEL_VERSION 2

// Colour depth per channel: 6 bits. The display's two frame buffers come out
// of the same memory the HTTPS price and block fetches need, and at the
// library's default 8 bits (~74 KB) the secure connections ran short
// ("BIGNUM - Memory allocation failed", "Generic error"). 6 bits (~55 KB)
// frees ~18 KB, and costs little: every colour is already stored as 5-6
// bits per channel (RGB565) before it reaches the panel.
//
// The depth MUST be set in the file build_opt.h, in this sketch's folder,
// which holds one line:   -DPIXEL_COLOR_DEPTH_BITS=6
// The HUB75 library picks its colour-correction table when the library
// itself is compiled, so a depth set only here in the sketch leaves it on
// its 8-bit table and mixed colours come out wrong. build_opt.h reaches the
// library's compile too. After adding or changing it, restart the Arduino
// IDE so the library is rebuilt.
#ifndef PIXEL_COLOR_DEPTH_BITS
#error "Colour depth not set: put build_opt.h (one line: -DPIXEL_COLOR_DEPTH_BITS=6) in this sketch's folder, then restart the Arduino IDE so the library is rebuilt"
#endif

// ---------- Timing ----------
// How often the price and block height are checked is set on the web
// control page (30 s to 10 min, default 5 min) — see updateIntervalMs().
#define WIFI_TIMEOUT_MS      15000UL  // 15 s per connection attempt (see WIFI_CONNECT_ATTEMPTS)
// Wi-Fi lost while running: BlockGrid keeps the last data on screen and
// keeps trying the home network — 15 s, then 30 s, then every minute between
// fresh attempts. After WIFI_FALLBACK_SETUP_MS without it, it switches to
// Wi-Fi setup mode, so a network that's gone for good (a phone hotspot,
// say) can be replaced. Setup mode keeps retrying the saved network every
// AP_RETRY_SAVED_MS and restarts normally when it answers, so a router
// restart still sorts itself out.
#define WIFI_RETRY_FIRST_MS   15000UL
#define WIFI_RETRY_MAX_MS     60000UL
#define WIFI_LOST_GRACE_MS     3000UL  // ignore blips shorter than this
#define WIFI_FALLBACK_SETUP_MS (3UL * 60UL * 1000UL)   // offline this long → setup mode
// In Wi-Fi setup mode with a saved network (it couldn't connect at boot,
// e.g. the router was still starting after a power cut): try the saved
// network again this often, for WIFI_TIMEOUT_MS each time, and restart
// normally once it answers.
#define AP_RETRY_SAVED_MS    120000UL
#define HTTP_TIMEOUT_MS       5000UL  // 5 s per-request timeout
// Boot splash timing — see the phase-by-phase comment in setup() for how
// these fit together (resolve, then fetch, then hold-while-raining,
// then gradual fade-out, then hold on the text alone).
#define SPLASH_POST_RESOLVE_HOLD_MS 2400UL // rain keeps flowing for this long after the word finishes decoding
#define RAIN_FADEOUT_MS             1100UL // existing rain dies out over this long (no new glyphs spawn); the GRID sweep plays at the start of it
#define RAIN_TEXT_ALONE_HOLD_MS     2100UL // once the rain's gone, hold on the bare word alone this long before switching to real data

// Sanity floor for block height responses. Bitcoin's height only increases;
// this just guards against displaying 0 / garbage on a bad response. Bump it
// up over time if you want a tighter check — it doesn't need to be exact.
#define MIN_PLAUSIBLE_BLOCK_HEIGHT 850000UL

// ---------- Image / logo ----------
typedef struct {
    const uint16_t *data;
    uint16_t width;
    uint16_t height;
    uint8_t dataSize;
} tImage;

// Bitcoin logo, 21x28 RGB565 — generated from the official high-res PNG
// with a subtle bevel (2px bevel, 30% highlight / 30% shadow) computed at
// full resolution, then area-averaged down so edges stay anti-aliased.
// Light from the top-left, matching the blockchain blocks. Identical to
// LOGO_DATA in matrix_sim_v3_1.html.
static const uint16_t image_data_logo[588] = {
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xB3C6, 0xB3A5, 0x51A1, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0820, 0xF549, 0xDC44, 0x6A01, 0x0000, 0x59E3, 0xB3A5, 0x7222, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x1060, 0x0020, 0x0000, 0x0000, 0x0000, 0x3942, 0xFD49, 0xC3A2, 0x3900, 0x0000, 0xA386, 0xF4E7, 0xBB62, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0xA386, 0xECE7, 0xB3A5, 0x7263, 0x3121, 0x82C5, 0xF508, 0xBB62, 0x0840, 0x0000, 0xE4C9, 0xDC24, 0x7A41, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0xE4A7, 0xECA5, 0xF4C5, 0xFD07, 0xFD27, 0xFD6A, 0xECC6, 0xCBC2, 0x51A2, 0x3942, 0xFD6A, 0xCBC3, 0x4961, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0020, 0x7221, 0xA302, 0xE443, 0xFCE5, 0xFD27, 0xFD28, 0xE464, 0xF4C5, 0xFD07, 0xFD49, 0xFD49, 0xCBC2, 0x4961, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x6A02, 0xFD07, 0xFD07, 0xCBC2, 0xB342, 0xCBC2, 0xDC23, 0xEC85, 0xEC85, 0xF484, 0xFCE6, 0xBBC4, 0x3921, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x51A3, 0xFD6A, 0xF507, 0xBB82, 0xB362, 0x61E1, 0x92A1, 0xCBC2, 0xDC02, 0xEC63, 0xF4E5, 0xFCE5, 0xF4C4, 0x8282, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x8B05, 0xFD6A, 0xECC6, 0xBB62, 0xA302, 0x0000, 0x0000, 0x0020, 0x51A1, 0xDC02, 0xF4A3, 0xFCE5, 0xF4A4, 0xEC63, 0x59C1,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xCC68, 0xFD6A, 0xDC44, 0xBB62, 0x7221, 0x0000, 0x0000, 0x0000, 0x0000, 0x20A0, 0xECA5, 0xFD06, 0xF4A4, 0xDC22, 0xBB62,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1060, 0xF56A, 0xFD6A, 0xCBE3, 0xBB62, 0x4961, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xBBE6, 0xFD49, 0xECA5, 0xCBA2, 0xD3E2,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x49A2, 0xFD6A, 0xF528, 0xC382, 0xBB62, 0x1880, 0x0000, 0x0000, 0x0000, 0x0000, 0x0020, 0xE4E9, 0xFD6A, 0xE464, 0xBB62, 0xAB22,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x8B05, 0xFD6A, 0xF507, 0xD3E2, 0xE423, 0x9B24, 0x61E2, 0x3101, 0x20A1, 0x3922, 0xABC7, 0xFD8B, 0xF528, 0xC382, 0xB342, 0x7201,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xCC48, 0xFD6A, 0xE465, 0xBB62, 0xDC03, 0xECA5, 0xF506, 0xFD28, 0xFD49, 0xFD6A, 0xFD8B, 0xED08, 0xBB82, 0xB342, 0xA302, 0x1060,
    0x0000, 0x0000, 0x0000, 0x0000, 0x1040, 0xF56A, 0xFD6A, 0xCBC3, 0xB342, 0xA2E2, 0xC3A2, 0xD402, 0xE423, 0xEC84, 0xF4E6, 0xF507, 0xC3A2, 0xBB62, 0x7221, 0x1060, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x4982, 0xFD6A, 0xF528, 0xBB82, 0xBB62, 0x1880, 0x0000, 0x28C0, 0x6A01, 0xC3A2, 0xE443, 0xF463, 0xDC23, 0xF483, 0xC3C3, 0x1880, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x8AE5, 0xFD6A, 0xECC6, 0xBB62, 0xAB22, 0x0000, 0x0000, 0x0000, 0x0000, 0x0020, 0x8281, 0xF4A3, 0xFCC5, 0xFCC5, 0xF4A3, 0xC3A2, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0xC448, 0xFD6A, 0xDC45, 0xBB62, 0x7A41, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xB384, 0xFD07, 0xFCE5, 0xE443, 0xDC02, 0x4140,
    0x0000, 0x9325, 0x6202, 0x59E3, 0xF56B, 0xFD6A, 0xCBE3, 0xBB62, 0x4961, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x7AA4, 0xFD49, 0xFD07, 0xD3E2, 0xC3A2, 0x61C1,
    0x51C3, 0xFD6A, 0xFD28, 0xFD6A, 0xFD8B, 0xF528, 0xC3A2, 0xBB82, 0x1880, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xBC08, 0xFD6A, 0xF506, 0xC3A2, 0xBB62, 0x4961,
    0xBBC5, 0xEC84, 0xDC23, 0xE464, 0xE464, 0xE443, 0xDC02, 0xEC63, 0xDC66, 0x9B24, 0x6203, 0x3942, 0x3101, 0x4982, 0xABA7, 0xFD8B, 0xFD8A, 0xEC84, 0xBB62, 0xB362, 0x1880,
    0x20A0, 0x59A1, 0x8AA1, 0xC3A2, 0xD3E2, 0xEC83, 0xD3E2, 0xDC23, 0xF4C5, 0xFD07, 0xFD28, 0xFD28, 0xFD49, 0xFD6A, 0xFD8B, 0xFD8B, 0xECC6, 0xC382, 0xAB42, 0x8A81, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x51A2, 0xFD28, 0xBB82, 0xC3A2, 0xD3E2, 0xE443, 0xEC84, 0xDC24, 0xE485, 0xECA5, 0xE485, 0xD403, 0xBB62, 0xAB42, 0xAB22, 0x2080, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x7264, 0xF508, 0xBB82, 0x1880, 0x3900, 0xECC6, 0xD424, 0xBB62, 0xCBC2, 0xCBC2, 0xC3A2, 0xBB82, 0xBB62, 0x92A1, 0x20A0, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0xABC7, 0xE4A6, 0xA302, 0x0000, 0x1880, 0xFD6A, 0xCBE3, 0x59A1, 0x1880, 0x3100, 0x4120, 0x3900, 0x1880, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0xE4C7, 0xD3E3, 0x7221, 0x0000, 0x51C3, 0xF549, 0xBB82, 0x28C0, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0820, 0x6A01, 0x92C2, 0x4120, 0x0000, 0x9325, 0xE485, 0xB342, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x59C1, 0x9AE2, 0x8261, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
};

const tImage logo = { image_data_logo, 21, 28, 16 };

// ビットコイン (Bittokoin, "Bitcoin" in katakana) — the authentic way
// Bitcoin is written in Japan; there's no kanji equivalent for it, and
// katakana is the correct form (loanwords use katakana). Sourced from the
// Misaki font's embedded bitmaps — same authoritative source as the 万/円
// icons — one 8x8 glyph per character (ビ, ッ, ト, コ, イ, ン), assembled
// into a single 48x8 wordmark image, white on black to match "ITCOIN"'s
// title color. 6 chars × 8px = 48px fits comfortably in the 72px title
// field (x=24 to x=96).
static const uint16_t image_data_bitcoinKatakana[384] = {
  0x0000, 0xFFFF, 0x0000, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000,
  0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000,
  0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000,
  0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000, 0xFFFF, 0xFFFF, 0xFFFF, 0x0000, 0x0000, 0x0000, 0x0000,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
};
const tImage bitcoinKatakanaImg = { image_data_bitcoinKatakana, 48, 8, 16 };

// ---------- Brand and currency cycling (two independent buttons) ----------
// Brand (logo + title, drawn at x=1,y=0 and x=24,y=6) and currency (which
// price/sats format the body uses) are independent choices — the brand
// button cycles one, the currency button cycles the other, so N brands and
// M currencies need N+M entries total instead of N×M combined ones.
enum class Currency : uint8_t { USD, JPY, EUR };   // saved in Preferences by this value: add new ones at the end

// Bundles one fetch cycle's results so displayBitcoinInfo() can pick USD
// or JPY figures based on the current mode without a long argument list.
// Defined up here (not down near where it's used) because Arduino's build
// system auto-generates a forward prototype for every function — including
// displayBitcoinInfo(const PriceSnapshot&) — and inserts those prototypes
// very early in the file. If PriceSnapshot were still defined near its one
// call site, that auto-generated prototype would reference a type that
// doesn't exist yet at that point in the file, and the whole sketch would
// fail to compile with "PriceSnapshot does not name a type".
struct PriceSnapshot {
    float    priceUSD;
    float    satsPerUSD;
    float    priceJPY;
    float    satsPer100Yen;
    uint32_t blockHeight;
    float    changePctUSD; // 24h % change (signed) — tracked separately per currency,
    float    changePctJPY; // since CoinGecko returns independent values for each.
                           // Up/down is simply change >= 0, derived where drawn.
    float    priceEUR;
    float    satsPerEUR;
    float    changePctEUR;
};

// Drives the new-block arrival animation in displayBitcoinInfo() (see the
// block-row section there) — passed as an optional pointer so every
// existing call site with just a PriceSnapshot still compiles unchanged
// (defaults to nullptr, the normal static rendering). Defined here rather
// than near playNewBlockAnimation() below for the same reason
// PriceSnapshot is: displayBitcoinInfo()'s auto-generated prototype needs
// this type to already exist at that point in the file.
struct BlockAnim {
    uint32_t oldValue;
    uint32_t newValue;
    unsigned long startMs;
};
#define BLOCK_ANIM_DURATION_MS 1700UL

// Explicit forward declaration — Arduino's auto-prototype generator
// (a simple parser, not a full C++ one) can silently fail to produce a
// working prototype for a function whose signature includes a default
// argument value, especially a pointer default like this one. When that
// happens it just skips the function rather than erroring at
// prototype-generation time, leaving any earlier call site (setup(),
// here) with no declaration at all and a "was not declared in this
// scope" error. Declaring it explicitly sidesteps the auto-generator
// entirely for this one function.
void displayBitcoinInfo(const PriceSnapshot &snap, BlockAnim *anim = nullptr);
PriceSnapshot currentSnapshot();   // declared explicitly, same reason as above

// A trimmed column range within a shared source bitmap — used to skip a
// glyph's built-in cell padding when packing multiple characters tightly.
struct GlyphRegion { uint8_t x, w; };

// One line of a small multi-line title (for a brand whose name doesn't fit
// one line at size 2), size 1 text at its own position.
struct TitleLine { const char *text; int16_t x, y; };

struct BrandProfile {
    const tImage *logo;                    // nullptr = draw the placeholder mark instead
    const char *title;                     // printed at x=24, same slot as "ITCOIN"
    uint8_t titleR, titleG, titleB;
    const tImage *titleImage = nullptr;    // if set, drawn instead of printing `title` as text
    const GlyphRegion *titleRegions = nullptr;  // if set, use the enlarged trimmed-region path
    uint8_t titleRegionCount = 0;
    float titleScale = 1.0f;
    uint8_t titleGap = 0;
    bool titleBevel = false;               // draw the title with the subtle bevel (see drawTitleBeveled())
    int16_t logoX = 1, logoY = 0;          // where the logo's top-left corner goes
    int16_t titleX = 24;                   // x of a text title (the same TITLE_Y row as "ITCOIN")
    const TitleLine *titleLines = nullptr; // if set, small (size 1) text lines instead of `title`
    uint8_t titleLineCount = 0;
};


// Title length note: at text size 2 (12px/char) there's 72px of width for
// the title (x=24 to x=96), which fits 6 characters — same budget "ITCOIN"
// uses. Longer names will need a smaller text size or will run off the
// right edge; ask if you want auto-scaling added later. titleImage-based
// titles (like the katakana one below) aren't bound to that per-character
// budget — just whatever the bitmap's own width is.
//
// Trimmed column ranges within the 48px-wide katakana source bitmap for
// each of the 6 characters (ビッコイン) — skips each glyph's blank cell
// padding so more of the panel's width goes to actual content, letting
// the scale go up from ~2x to ~2.4x. See drawScaledRegions().
const GlyphRegion katakanaRegions[] = {
    {1,6}, {9,5}, {18,4}, {25,6}, {33,6}, {41,6}
};

// Slot 1: your existing Bitcoin branding.
// Slot 2: ビットコイン (Bitcoin in katakana) — no logo, so the enlarged
// wordmark gets the full width; image-based title using the trimmed-
// region scaled path instead of ASCII text.
// Both titles use the subtle bevel, matching the beveled Bitcoin logo (the
// title colour below is then only a fallback; the bevel sets its own shades).

const BrandProfile brandProfiles[] = {
    { &logo,   "ITCOIN", 215, 215, 215, nullptr, nullptr, 0, 1.0f, 0, true },
    { nullptr, "",       255, 255, 255, &bitcoinKatakanaImg, katakanaRegions, 6, 92.0f/38.0f, 1, true },
};
const uint8_t NUM_BRAND_PROFILES = sizeof(brandProfiles) / sizeof(brandProfiles[0]);

// Display names for the web control page, one per brandProfiles[] entry.
const char *const brandNames[] = { "Bitcoin", "ビットコイン (Bitcoin)" };
static_assert(sizeof(brandNames) / sizeof(brandNames[0]) == sizeof(brandProfiles) / sizeof(brandProfiles[0]),
              "brandNames[] needs one name per brandProfiles[] entry");

// USD keeps the original layout; JPY shows a man-scaled (万, units of
// 10,000) number plus a small icon instead of spelling the full 8-digit
// yen amount. Add more entries here as needed — displayBitcoinInfo()
// branches on the Currency value.
const Currency currencyOptions[] = { Currency::USD, Currency::EUR, Currency::JPY };   // the order the button and page cycle through
const uint8_t NUM_CURRENCY_OPTIONS = sizeof(currencyOptions) / sizeof(currencyOptions[0]);
const char *currencyCode(Currency c) { return c == Currency::JPY ? "JPY" : c == Currency::EUR ? "EUR" : "USD"; }
// Where a currency saved as its Currency value sits in currencyOptions[] (0 if unknown).
uint8_t currencyIndexOf(uint8_t saved) {
    for (uint8_t i = 0; i < NUM_CURRENCY_OPTIONS; i++) if ((uint8_t)currencyOptions[i] == saved) return i;
    return 0;
}

// 万 (man, units of 10,000) icon for JPY mode — 8x8, pure on/off strokes
// (no anti-aliasing), sourced from the actual embedded bitmap in the
// Misaki font (littlelimit.net) — an 8x8 Japanese font hand-designed
// specifically for tiny displays, originally for the Sharp PC-E500 pocket
// computer. Using its real pixel data rather than downsampling a filled
// glyph (which produced a blurry halo) or guessing the strokes by hand.
// Source bytes (one per row, MSB=leftmost column): 0xFE, 0x20, 0x3C, 0x24,
// 0x24, 0x44, 0x8C, 0x00.
static const uint16_t image_data_manGlyph[64] = {
  0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x0000, 0x0000, 0x0000, 0x07E0, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x0000, 0x0000, 0x0000, 0x0000, 0x07E0, 0x0000, 0x0000, 0x07E0, 0x0000, 0x0000, 0x0000, 0x0000, 0x07E0, 0x0000, 0x0000, 0x07E0, 0x0000, 0x0000, 0x0000, 0x07E0, 0x0000, 0x0000, 0x0000, 0x07E0, 0x0000, 0x0000, 0x07E0, 0x0000, 0x0000, 0x0000, 0x07E0, 0x07E0, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000
};

// Icon for the JPY man (万, units of 10,000) unit. Drawn inline right after
// the price digits (not a fixed position) so it always sits immediately
// after the last digit, however many digits the price grows to.
const tImage manGlyphImg = { image_data_manGlyph, 8, 8, 16 };
const tImage *manGlyph = &manGlyphImg;

// ¥ is printed as text rather than a custom bitmap: the panel font
// (BlockGridFont5x7, below) has a ¥ glyph at 0x9D, the same code Adafruit
// GFX's built-in font uses for it. See where it's printed in
// displayBitcoinInfo() via write((uint8_t)0x9D).

// The display driver object — declared here (not down in the usual
// "Globals" section) specifically so it's visible to drawPlaceholderBox()
// and drawScaledRegions() below, which are defined earlier in the file
// than where the rest of the globals live. Arduino's build system doesn't
// reorder global variable declarations the way it does function
// prototypes, so this one has to physically come first.
MatrixPanel_I2S_DMA *dma_display = nullptr;

// ---------- Text font (matches the simulator exactly) ----------
// Every piece of text on the panel is drawn with this font instead of
// Adafruit GFX's built-in one, so the device shows exactly what the
// simulator shows. Generated from the simulator's FONT5x7 table: 5x7
// glyphs, 6 px per character (4 px for a space), drawn from the cursor's
// top-left corner just like the built-in font — so every setCursor() and
// setTextSize() keeps working unchanged. Covers ' ' to '~' plus ¥ (0x9D,
// the same code the built-in font used); 0x7F-0x9C are empty.
// To change a letter, change it in the simulator first, then regenerate this.
const uint8_t BlockGridFont5x7Bitmaps[] PROGMEM = {
    0x21, 0x08, 0x42, 0x00, 0x80, 0x52, 0x94, 0x00, 0x00, 0x00, 0x52, 0xBE, 0xAF, 0xA9, 0x40, 0x23,
    0xE8, 0xE2, 0xF8, 0x80, 0xC6, 0x44, 0x44, 0x4C, 0x60, 0x45, 0x28, 0x8A, 0xC9, 0xA0, 0x31, 0x88,
    0x80, 0x00, 0x00, 0x11, 0x10, 0x84, 0x10, 0x40, 0x41, 0x04, 0x21, 0x11, 0x00, 0x25, 0x5D, 0xF7,
    0x54, 0x80, 0x01, 0x09, 0xF2, 0x10, 0x00, 0x00, 0x00, 0x06, 0x11, 0x00, 0x00, 0x01, 0xF0, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x31, 0x80, 0x00, 0x44, 0x44, 0x40, 0x00, 0x74, 0x67, 0x5C, 0xC5, 0xC0,
    0x23, 0x08, 0x42, 0x11, 0xC0, 0x74, 0x42, 0x22, 0x23, 0xE0, 0xF8, 0x88, 0x20, 0xC5, 0xC0, 0x11,
    0x95, 0x2F, 0x88, 0x40, 0xFC, 0x3C, 0x10, 0xC5, 0xC0, 0x32, 0x21, 0xE8, 0xC5, 0xC0, 0xF8, 0x44,
    0x44, 0x21, 0x00, 0x74, 0x62, 0xE8, 0xC5, 0xC0, 0x74, 0x62, 0xF0, 0x89, 0x80, 0x03, 0x18, 0x06,
    0x30, 0x00, 0x00, 0x08, 0x02, 0x11, 0x00, 0x08, 0x88, 0x82, 0x08, 0x20, 0x00, 0x3E, 0x0F, 0x80,
    0x00, 0x41, 0x04, 0x11, 0x11, 0x00, 0x74, 0x42, 0x62, 0x00, 0x80, 0x74, 0x6B, 0x7B, 0x41, 0xE0,
    0x74, 0x63, 0x1F, 0xC6, 0x20, 0xF4, 0x63, 0xE8, 0xC7, 0xC0, 0x74, 0x61, 0x08, 0x45, 0xC0, 0xE4,
    0xA3, 0x18, 0xCB, 0x80, 0xFC, 0x21, 0xE8, 0x43, 0xE0, 0xFC, 0x21, 0xE8, 0x42, 0x00, 0x74, 0x61,
    0x78, 0xC5, 0xE0, 0x8C, 0x63, 0xF8, 0xC6, 0x20, 0x71, 0x08, 0x42, 0x11, 0xC0, 0x10, 0x84, 0x21,
    0x49, 0x80, 0x8C, 0xA9, 0x8A, 0x4A, 0x20, 0x84, 0x21, 0x08, 0x43, 0xE0, 0x8E, 0xEB, 0x58, 0xC6,
    0x20, 0x8C, 0x73, 0x59, 0xC6, 0x20, 0x74, 0x63, 0x18, 0xC5, 0xC0, 0xF4, 0x63, 0xE8, 0x42, 0x00,
    0x74, 0x63, 0x1A, 0xC9, 0xA0, 0xF4, 0x63, 0xEA, 0x4A, 0x20, 0x7C, 0x20, 0xE0, 0x87, 0xC0, 0xF9,
    0x08, 0x42, 0x10, 0x80, 0x8C, 0x63, 0x18, 0xC5, 0xC0, 0x8C, 0x63, 0x18, 0xA8, 0x80, 0x8C, 0x63,
    0x5A, 0xD5, 0x40, 0x8C, 0x54, 0x45, 0x46, 0x20, 0x8C, 0x62, 0xA2, 0x10, 0x80, 0xF8, 0x44, 0x44,
    0x43, 0xE0, 0x7A, 0x10, 0x84, 0x21, 0xE0, 0x04, 0x10, 0x41, 0x04, 0x00, 0x78, 0x42, 0x10, 0x85,
    0xE0, 0x22, 0xA2, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xE0, 0x63, 0x08, 0x20, 0x00, 0x00,
    0x00, 0x1C, 0x17, 0xC5, 0xE0, 0x84, 0x2D, 0x98, 0xC7, 0xC0, 0x00, 0x1D, 0x08, 0x45, 0xC0, 0x08,
    0x5B, 0x38, 0xC5, 0xE0, 0x00, 0x1D, 0x1F, 0xC1, 0xC0, 0x32, 0x51, 0xC4, 0x21, 0x00, 0x00, 0x1F,
    0x17, 0x85, 0xC0, 0x84, 0x2D, 0x98, 0xC6, 0x20, 0x20, 0x18, 0x42, 0x11, 0xC0, 0x10, 0x0C, 0x21,
    0x49, 0x80, 0x84, 0x25, 0x4C, 0x52, 0x40, 0x61, 0x08, 0x42, 0x11, 0xC0, 0x00, 0x35, 0x5A, 0xC6,
    0x20, 0x00, 0x2D, 0x98, 0xC6, 0x20, 0x00, 0x1D, 0x18, 0xC5, 0xC0, 0x00, 0x3D, 0x1F, 0x42, 0x00,
    0x00, 0x1B, 0x37, 0x84, 0x20, 0x00, 0x2D, 0x98, 0x42, 0x00, 0x00, 0x1D, 0x07, 0x07, 0xC0, 0x42,
    0x38, 0x84, 0x24, 0xC0, 0x00, 0x23, 0x18, 0xCD, 0xA0, 0x00, 0x23, 0x18, 0xA8, 0x80, 0x00, 0x23,
    0x1A, 0xD5, 0x40, 0x00, 0x22, 0xA2, 0x2A, 0x20, 0x00, 0x23, 0x17, 0x85, 0xC0, 0x00, 0x3E, 0x22,
    0x23, 0xE0, 0x11, 0x08, 0x82, 0x10, 0x40, 0x21, 0x08, 0x02, 0x10, 0x80, 0x41, 0x08, 0x22, 0x11,
    0x00, 0x45, 0x44, 0x00, 0x00, 0x00, 0xDE, 0xDD, 0xF2, 0x7C, 0x80,
    0x3A, 0x3C, 0x8F, 0x20, 0xE0,   // € (0x9E)
};
const GFXglyph BlockGridFont5x7Glyphs[] PROGMEM = {
    {    0, 0, 0, 4, 0, 0 },   // ' '
    {    0, 5, 7, 6, 0, 0 },   // '!'
    {    5, 5, 7, 6, 0, 0 },   // '"'
    {   10, 5, 7, 6, 0, 0 },   // '#'
    {   15, 5, 7, 6, 0, 0 },   // '$'
    {   20, 5, 7, 6, 0, 0 },   // '%'
    {   25, 5, 7, 6, 0, 0 },   // '&'
    {   30, 5, 7, 6, 0, 0 },   // apostrophe
    {   35, 5, 7, 6, 0, 0 },   // '('
    {   40, 5, 7, 6, 0, 0 },   // ')'
    {   45, 5, 7, 6, 0, 0 },   // '*'
    {   50, 5, 7, 6, 0, 0 },   // '+'
    {   55, 5, 7, 6, 0, 0 },   // ','
    {   60, 5, 7, 6, 0, 0 },   // '-'
    {   65, 5, 7, 6, 0, 0 },   // '.'
    {   70, 5, 7, 6, 0, 0 },   // '/'
    {   75, 5, 7, 6, 0, 0 },   // '0'
    {   80, 5, 7, 6, 0, 0 },   // '1'
    {   85, 5, 7, 6, 0, 0 },   // '2'
    {   90, 5, 7, 6, 0, 0 },   // '3'
    {   95, 5, 7, 6, 0, 0 },   // '4'
    {  100, 5, 7, 6, 0, 0 },   // '5'
    {  105, 5, 7, 6, 0, 0 },   // '6'
    {  110, 5, 7, 6, 0, 0 },   // '7'
    {  115, 5, 7, 6, 0, 0 },   // '8'
    {  120, 5, 7, 6, 0, 0 },   // '9'
    {  125, 5, 7, 6, 0, 0 },   // ':'
    {  130, 5, 7, 6, 0, 0 },   // ';'
    {  135, 5, 7, 6, 0, 0 },   // '<'
    {  140, 5, 7, 6, 0, 0 },   // '='
    {  145, 5, 7, 6, 0, 0 },   // '>'
    {  150, 5, 7, 6, 0, 0 },   // '?'
    {  155, 5, 7, 6, 0, 0 },   // '@'
    {  160, 5, 7, 6, 0, 0 },   // 'A'
    {  165, 5, 7, 6, 0, 0 },   // 'B'
    {  170, 5, 7, 6, 0, 0 },   // 'C'
    {  175, 5, 7, 6, 0, 0 },   // 'D'
    {  180, 5, 7, 6, 0, 0 },   // 'E'
    {  185, 5, 7, 6, 0, 0 },   // 'F'
    {  190, 5, 7, 6, 0, 0 },   // 'G'
    {  195, 5, 7, 6, 0, 0 },   // 'H'
    {  200, 5, 7, 6, 0, 0 },   // 'I'
    {  205, 5, 7, 6, 0, 0 },   // 'J'
    {  210, 5, 7, 6, 0, 0 },   // 'K'
    {  215, 5, 7, 6, 0, 0 },   // 'L'
    {  220, 5, 7, 6, 0, 0 },   // 'M'
    {  225, 5, 7, 6, 0, 0 },   // 'N'
    {  230, 5, 7, 6, 0, 0 },   // 'O'
    {  235, 5, 7, 6, 0, 0 },   // 'P'
    {  240, 5, 7, 6, 0, 0 },   // 'Q'
    {  245, 5, 7, 6, 0, 0 },   // 'R'
    {  250, 5, 7, 6, 0, 0 },   // 'S'
    {  255, 5, 7, 6, 0, 0 },   // 'T'
    {  260, 5, 7, 6, 0, 0 },   // 'U'
    {  265, 5, 7, 6, 0, 0 },   // 'V'
    {  270, 5, 7, 6, 0, 0 },   // 'W'
    {  275, 5, 7, 6, 0, 0 },   // 'X'
    {  280, 5, 7, 6, 0, 0 },   // 'Y'
    {  285, 5, 7, 6, 0, 0 },   // 'Z'
    {  290, 5, 7, 6, 0, 0 },   // '['
    {  295, 5, 7, 6, 0, 0 },   // backslash
    {  300, 5, 7, 6, 0, 0 },   // ']'
    {  305, 5, 7, 6, 0, 0 },   // '^'
    {  310, 5, 7, 6, 0, 0 },   // '_'
    {  315, 5, 7, 6, 0, 0 },   // '`'
    {  320, 5, 7, 6, 0, 0 },   // 'a'
    {  325, 5, 7, 6, 0, 0 },   // 'b'
    {  330, 5, 7, 6, 0, 0 },   // 'c'
    {  335, 5, 7, 6, 0, 0 },   // 'd'
    {  340, 5, 7, 6, 0, 0 },   // 'e'
    {  345, 5, 7, 6, 0, 0 },   // 'f'
    {  350, 5, 7, 6, 0, 0 },   // 'g'
    {  355, 5, 7, 6, 0, 0 },   // 'h'
    {  360, 5, 7, 6, 0, 0 },   // 'i'
    {  365, 5, 7, 6, 0, 0 },   // 'j'
    {  370, 5, 7, 6, 0, 0 },   // 'k'
    {  375, 5, 7, 6, 0, 0 },   // 'l'
    {  380, 5, 7, 6, 0, 0 },   // 'm'
    {  385, 5, 7, 6, 0, 0 },   // 'n'
    {  390, 5, 7, 6, 0, 0 },   // 'o'
    {  395, 5, 7, 6, 0, 0 },   // 'p'
    {  400, 5, 7, 6, 0, 0 },   // 'q'
    {  405, 5, 7, 6, 0, 0 },   // 'r'
    {  410, 5, 7, 6, 0, 0 },   // 's'
    {  415, 5, 7, 6, 0, 0 },   // 't'
    {  420, 5, 7, 6, 0, 0 },   // 'u'
    {  425, 5, 7, 6, 0, 0 },   // 'v'
    {  430, 5, 7, 6, 0, 0 },   // 'w'
    {  435, 5, 7, 6, 0, 0 },   // 'x'
    {  440, 5, 7, 6, 0, 0 },   // 'y'
    {  445, 5, 7, 6, 0, 0 },   // 'z'
    {  450, 5, 7, 6, 0, 0 },   // '{'
    {  455, 5, 7, 6, 0, 0 },   // '|'
    {  460, 5, 7, 6, 0, 0 },   // '}'
    {  465, 5, 7, 6, 0, 0 },   // '~'
    {  470, 0, 0, 6, 0, 0 },   // 0x7F unused
    {  470, 0, 0, 6, 0, 0 },   // 0x80 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x81 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x82 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x83 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x84 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x85 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x86 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x87 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x88 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x89 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x8A unused
    {  470, 0, 0, 6, 0, 0 },   // 0x8B unused
    {  470, 0, 0, 6, 0, 0 },   // 0x8C unused
    {  470, 0, 0, 6, 0, 0 },   // 0x8D unused
    {  470, 0, 0, 6, 0, 0 },   // 0x8E unused
    {  470, 0, 0, 6, 0, 0 },   // 0x8F unused
    {  470, 0, 0, 6, 0, 0 },   // 0x90 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x91 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x92 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x93 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x94 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x95 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x96 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x97 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x98 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x99 unused
    {  470, 0, 0, 6, 0, 0 },   // 0x9A unused
    {  470, 0, 0, 6, 0, 0 },   // 0x9B unused
    {  470, 0, 0, 6, 0, 0 },   // 0x9C unused
    {  470, 5, 7, 6, 0, 0 },   // ¥ (0x9D)
    {  475, 5, 7, 6, 0, 0 },   // € (0x9E)
};
const GFXfont BlockGridFont5x7 PROGMEM = {
    (uint8_t *)BlockGridFont5x7Bitmaps, (GFXglyph *)BlockGridFont5x7Glyphs, 0x20, 0x9E, 8
};


// Generic placeholder mark for any not-yet-converted bitmap slot — a dim
// outline box with a centered "+". Used for both the brand-logo placeholder
// (21x28) and the smaller JPY man-icon placeholder (8x8).
void drawPlaceholderBox(int16_t x, int16_t y, int16_t w, int16_t h) {
    uint16_t c = dma_display->color565(120, 120, 120);
    dma_display->drawRect(x, y, w, h, c);
    if (w >= 6 && h >= 6) {
        int16_t midX = x + w / 2, midY = y + h / 2;
        dma_display->drawLine(midX, y + 2, midX, y + h - 3, c);
        dma_display->drawLine(x + 2, midY, x + w - 3, midY, c);
    }
}

// 3-pixel stub before the chain, implying blocks even older than the
// oldest one actually drawn continue off-screen to the left.
void drawChainStub(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b) {
    dma_display->drawFastHLine(x, y + 3, 3, dma_display->color565(r, g, b));
}

// Three linked 3D-beveled blocks — replaces the word "Block" as the label
// for the block height row. Each of the three gets its own color — first
// (oldest) darkest, middle in between, third (newest) brightest/current —
// reading as a gradient fading into history rather than a hard cut
// between "old" and "current." Ported from the simulator's current
// drawBlockchainIcon()/drawChainStub() (matrix_sim_v3_1.html), designed
// and verified there first. Same bevel technique as before (bright
// highlight on top/left edges, dark shadow on bottom/right, filled
// interior), computed per-block since each one can have an entirely
// different base color. Each link matches the block it's leaving from
// (first link = first block's color, second link = middle block's
// color), so the gradient reads continuously along the whole chain
// rather than jumping at the connectors.
void drawBlockchainIcon(int16_t x0, int16_t y0,
                         uint8_t r, uint8_t g, uint8_t b,
                         uint8_t midR, uint8_t midG, uint8_t midB,
                         uint8_t newestR, uint8_t newestG, uint8_t newestB) {
    auto block = [&](int16_t bx, uint8_t br, uint8_t bg, uint8_t bb) {
        uint16_t hiColor = dma_display->color565(
            (uint8_t)(br + (255 - br) * 0.55f + 0.5f),
            (uint8_t)(bg + (255 - bg) * 0.55f + 0.5f),
            (uint8_t)(bb + (255 - bb) * 0.55f + 0.5f));
        uint16_t shColor = dma_display->color565(
            (uint8_t)(br * 0.5f + 0.5f),
            (uint8_t)(bg * 0.5f + 0.5f),
            (uint8_t)(bb * 0.5f + 0.5f));
        uint16_t baseColor = dma_display->color565(br, bg, bb);
        dma_display->fillRect(bx + 1, y0 + 1, 5, 5, baseColor);  // filled interior
        dma_display->drawFastHLine(bx, y0, 7, hiColor);          // top edge (highlight)
        dma_display->drawFastVLine(bx, y0, 7, hiColor);          // left edge (highlight)
        dma_display->drawFastHLine(bx, y0 + 6, 7, shColor);      // bottom edge (shadow)
        dma_display->drawFastVLine(bx + 6, y0, 7, shColor);      // right edge (shadow)
    };

    block(x0, r, g, b);            // first (oldest visible) block
    dma_display->drawFastHLine(x0 + 7, y0 + 3, 3, dma_display->color565(r, g, b));
    block(x0 + 10, midR, midG, midB);       // second (middle) block
    dma_display->drawFastHLine(x0 + 17, y0 + 3, 3, dma_display->color565(midR, midG, midB));
    block(x0 + 20, newestR, newestG, newestB);  // third (newest) block — its own color
}

// ---------- Price-change effect ----------
// When a fetch brings a new price, the characters of the price, the 24h %
// and the Sat/$1 (Sat/¥100) figure that differ from before animate
// briefly; unchanged ones stay put. Chosen on the web control page, saved
// in Preferences ("priceFx"):
//   Roll  — odometer: changed characters roll in from below when that
//           number went up, from above when it went down
//   Flash (default) — changed characters flash and ease back to their
//           normal colour: pale green (price uptick) / pale red (downtick)
//           on the price row, bright white on the Sat row (whose number
//           moves opposite to the price)
//   Sweep — a soft light band glides once across the price row and the
//           Sat row
//   Off
// Mirrors the simulator's PRICE_FX / drawFxText() / applyPriceSweep().
enum PriceFxStyle : uint8_t { FX_ROLL, FX_FLASH, FX_SWEEP, FX_OFF, NUM_PRICE_FX };
const char *const priceFxNames[NUM_PRICE_FX] = { "Roll", "Flash", "Sweep", "Off" };
const uint16_t priceFxDurationMs[NUM_PRICE_FX] = { 450, 1000, 700, 0 };
uint8_t currentPriceFx = FX_FLASH;

// The previous strings are kept so each character can be compared with what
// was there before. '\x9D' is ¥ in the panel font.
struct PriceFxState {
    bool active = false;
    unsigned long startMs = 0;
    int8_t dir = 1;                        // price tick: +1 up, -1 down (flash colour, price roll)
    int8_t pctDirUsd = 1, pctDirJpy = 1, pctDirEur = 1;   // did the shown % magnitude go up or down (its roll)
    int8_t satDirUsd = 1, satDirJpy = 1, satDirEur = 1;   // did the sats figure go up or down (its roll)
    char oldUsd[16] = "", oldJpy[16] = "", oldPctUsd[12] = "", oldPctJpy[12] = "";
    char oldSatUsd[24] = "", oldSatJpy[24] = "";
    char oldEur[16] = "", oldPctEur[12] = "", oldSatEur[24] = "";
};
PriceFxState priceFx;

// Progress 0..1 of the running effect, or -1 when none is running.
float priceFxProgress() {
    if (!priceFx.active || currentPriceFx == FX_OFF) { priceFx.active = false; return -1.0f; }
    float p = (millis() - priceFx.startMs) / (float)priceFxDurationMs[currentPriceFx];
    if (p >= 1.0f) { priceFx.active = false; return -1.0f; }
    return p;
}
static inline float fxEase(float t) { return t * t * (3.0f - 2.0f * t); }

// The strings the price row shows — used both for drawing and for
// remembering what was shown before a fetch.
void fmtUsdPrice(char *buf, size_t n, float priceUSD) { snprintf(buf, n, "$%lu", (unsigned long)priceUSD); }
void fmtEurPrice(char *buf, size_t n, float priceEUR) { snprintf(buf, n, "\x9E%lu", (unsigned long)priceEUR); }   // '\x9E' is € in the panel font
void fmtJpyPrice(char *buf, size_t n, float priceJPY) { snprintf(buf, n, "\x9D%lu", (unsigned long)(priceJPY / 10000.0f + 0.5f)); }
void fmtPct(char *buf, size_t n, float changePct) {   // "2.3%" (magnitude only; the arrow carries the sign)
    if (isnan(changePct)) { buf[0] = 0; return; }
    uint32_t tenths = (uint32_t)(fabsf(changePct) * 10.0f + 0.5f);
    snprintf(buf, n, "%lu.%lu%%", (unsigned long)(tenths / 10), (unsigned long)(tenths % 10));
}

void fmtSatUsd(char *buf, size_t n, float satsPerUSD) {
    if (satsPerUSD > 0.0f) snprintf(buf, n, "Sat/$1:%lu", (unsigned long)satsPerUSD);
    else                   snprintf(buf, n, "Sat/$1:ERR");
}
void fmtSatEur(char *buf, size_t n, float satsPerEUR) {
    if (satsPerEUR > 0.0f) snprintf(buf, n, "Sat/\x9E" "1:%lu", (unsigned long)satsPerEUR);
    else                   snprintf(buf, n, "Sat/\x9E" "1:ERR");
}
void fmtSatJpy(char *buf, size_t n, float satsPer100Yen) {
    if (satsPer100Yen > 0.0f) snprintf(buf, n, "Sat/\x9D" "100:%lu", (unsigned long)satsPer100Yen);
    else                      snprintf(buf, n, "Sat/\x9D" "100:ERR");
}
const uint8_t SAT_RGB[3]       = { 255, 228, 190 };   // warm white — the Sat/$1 (Sat/¥100) row
const uint8_t SAT_FLASH_RGB[3] = { 255, 255, 255 };   // bright white — the Sat row's flash colour

// Sweep band for the row currently being drawn (see fxPixel()).
struct { bool on = false; float bx = 0; int16_t x0 = 0, x1 = 0; } fxSweep;

// Every pixel of the price row goes through here, so the sweep can
// brighten whatever is under its band (arrow, digits, decimal dot alike).
void fxPixel(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b) {
    if (fxSweep.on && x >= fxSweep.x0 && x < fxSweep.x1) {
        float k = 1.0f - fabsf(x - fxSweep.bx) / 3.5f;
        if (k > 0.0f) {
            k *= 0.75f;
            r = (uint8_t)(r + (255 - r) * k);
            g = (uint8_t)(g + (255 - g) * k);
            b = (uint8_t)(b + (255 - b) * k);
        }
    }
    dma_display->drawPixel(x, y, dma_display->color565(r, g, b));
}

// One size-1 character of the panel font, drawn pixel by pixel, keeping only
// rows clipY0 (inclusive) to clipY1 (exclusive) — for the odometer roll.
void fxGlyph(uint8_t c, int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b,
             int16_t clipY0 = -32768, int16_t clipY1 = 32767) {
    if (c < 0x20 || c > 0x9E) return;
    const GFXglyph &gl = BlockGridFont5x7Glyphs[c - 0x20];
    const uint8_t *bm = BlockGridFont5x7Bitmaps + gl.bitmapOffset;
    uint8_t bits = 0, bit = 0;
    for (uint8_t yy = 0; yy < gl.height; yy++) {
        for (uint8_t xx = 0; xx < gl.width; xx++) {
            if (!(bit++ & 7)) bits = *bm++;
            if (bits & 0x80) {
                int16_t py = y + yy;
                if (py >= clipY0 && py < clipY1) fxPixel(x + xx, py, r, g, b);
            }
            bits <<= 1;
        }
    }
}
uint8_t fxAdvance(uint8_t c) { return (c < 0x20 || c > 0x9E) ? 6 : BlockGridFont5x7Glyphs[c - 0x20].xAdvance; }

// Draws str at size 1, animating the characters that differ from oldStr
// (compared right-aligned, since numbers grow to the left) with the roll or
// flash effect. p < 0 or no oldStr = plain. rollDir +1 = number went up.
// Returns the x just past the last character.
// flashRGB: the flash colour; nullptr = pale green / pale red by the price tick.
int16_t drawFxText(const char *str, const char *oldStr, int16_t x, int16_t y,
                   uint8_t r, uint8_t g, uint8_t b, int8_t rollDir, float p,
                   const uint8_t *flashRGB = nullptr) {
    bool animate = p >= 0.0f && oldStr != nullptr && oldStr[0] && strcmp(str, oldStr) != 0 &&
                   (currentPriceFx == FX_ROLL || currentPriceFx == FX_FLASH);
    int16_t n = strlen(str), m = oldStr ? strlen(oldStr) : 0, shift = n - m;
    int16_t cx = x;
    for (int16_t i = 0; i < n; i++) {
        uint8_t ch = (uint8_t)str[i];
        int16_t j = i - shift;
        uint8_t was = (animate && j >= 0 && j < m) ? (uint8_t)oldStr[j] : ' ';
        if (!animate || ch == was) {
            fxGlyph(ch, cx, y, r, g, b);
        } else if (currentPriceFx == FX_FLASH) {
            float t = fxEase(p);
            uint8_t hr = priceFx.dir > 0 ? 210 : 255, hg = priceFx.dir > 0 ? 255 : 205, hb = priceFx.dir > 0 ? 215 : 205;
            if (flashRGB) { hr = flashRGB[0]; hg = flashRGB[1]; hb = flashRGB[2]; }
            fxGlyph(ch, cx, y, (uint8_t)lroundf(hr + (r - hr) * t), (uint8_t)lroundf(hg + (g - hg) * t),
                    (uint8_t)lroundf(hb + (b - hb) * t));
        } else {   // roll: 8 = 7 rows + 1 gap; "up" = new character comes in from below
            int16_t off = (int16_t)lroundf(8.0f * (1.0f - fxEase(p)));
            int16_t dy = rollDir > 0 ? off : -off;
            fxGlyph(ch,  cx, y + dy, r, g, b, y, y + 7);
            fxGlyph(was, cx, y + dy - (rollDir > 0 ? 8 : -8), r, g, b, y, y + 7);
        }
        cx += fxAdvance(ch);
    }
    return cx;
}

// Small solid triangle indicating price direction over the reference
// period (24h change, per fetchBitcoinPrices) — green pointing up for a
// gain, red pointing down for a loss. y is the row's own baseline; the
// triangle is vertically centered within the 7px glyph height on its own.
// Drawn as the first part of the 24h change indicator (see
// drawChangeIndicator below), not beside the price itself.
// Ported from the simulator's drawPriceArrow() (matrix_sim_v3_1.html).
void drawPriceArrow(int16_t x, int16_t y, bool up) {
    uint8_t r = up ? 0 : 255, g = up ? 255 : 60, b = up ? 70 : 60;
    // Pixel by pixel through fxPixel() so the sweep effect can light it too.
    static const uint8_t upRows[3][2]   = { {2, 1}, {1, 3}, {0, 5} };   // {first x, length} for rows y+2..y+4
    static const uint8_t downRows[3][2] = { {0, 5}, {1, 3}, {2, 1} };
    const uint8_t (*rows)[2] = up ? upRows : downRows;
    for (uint8_t i = 0; i < 3; i++)
        for (uint8_t k = 0; k < rows[i][1]; k++) fxPixel(x + rows[i][0] + k, y + 2 + i, r, g, b);
}

// 24h change indicator: ▲/▼ arrow followed by the magnitude, e.g. "▲2.3%",
// placed CHANGE_GAP pixels left of the price (priceX = the price's left
// edge, including its $/¥ symbol). The arrow carries the sign, so the
// number never needs "+" or "-". Both parts use the price's own up/down
// color. The decimal point is drawn as a narrow 2px dot rather than the
// font's '.', which sits in a full 6px cell and makes "2.3%" read as
// "2. 3%". Ported from the simulator's drawChangeIndicator() — same width
// math: 5px arrow + 2px gap + whole digits + 3px dot + 1 digit + '%'.
#define CHANGE_GAP 6
// Price color from the 24h change: green for up (or flat), red for down.
// Unknown (NAN) is explicitly green — the price's original color — because
// every comparison against NAN is false, so a plain "change >= 0 ? green :
// red" would silently turn the price red whenever the value is missing.
uint16_t priceDirectionColor(float changePct) {
    if (isnan(changePct) || changePct >= 0.0f) return dma_display->color565(0, 255, 70);
    return dma_display->color565(255, 60, 60);
}
uint8_t numDigits(uint32_t n);   // defined further down; declared here explicitly rather than relying on Arduino's auto-prototypes
int16_t changeIndicatorWidth(float changePct) {
    uint32_t tenths = (uint32_t)(fabsf(changePct) * 10.0f + 0.5f);
    return 5 + 2 + 6 * numDigits(tenths / 10) + 3 + 6 + 6;
}
// oldPct: the "2.3%" string shown before the last fetch (for the price-change
// effect), pctDir: whether its magnitude went up (+1) or down (-1).
void drawChangeIndicator(int16_t priceX, int16_t y, float changePct, const char *oldPct, int8_t pctDir, float p) {
    bool up = changePct >= 0.0f;
    uint8_t r = up ? 0 : 255, g = up ? 255 : 60, b = up ? 70 : 60;
    int16_t x = priceX - CHANGE_GAP - changeIndicatorWidth(changePct);

    drawPriceArrow(x, y, up);
    // Whole part and "tenth%" part animate separately (each right-aligned),
    // so the narrow decimal dot never moves.
    char pct[12]; fmtPct(pct, sizeof pct, changePct);
    char *dot = strchr(pct, '.');
    *dot = 0;
    char oldWhole[12] = "", *oldRest = nullptr;
    if (oldPct && oldPct[0]) {
        strncpy(oldWhole, oldPct, sizeof oldWhole - 1);
        char *od = strchr(oldWhole, '.');
        if (od) { *od = 0; oldRest = od + 1; }
    }
    int16_t cx = drawFxText(pct, oldPct && oldPct[0] ? oldWhole : nullptr, x + 5 + 2, y, r, g, b, pctDir, p);
    for (uint8_t dy = 5; dy <= 6; dy++)
        for (uint8_t dx = 0; dx <= 1; dx++) fxPixel(cx + dx, y + dy, r, g, b);   // narrow decimal point
    drawFxText(dot + 1, oldRest, cx + 3, y, r, g, b, pctDir, p);
}

// Sat/$1 (Sat/¥100) row, right-aligned to rightX, with the price-change
// effect (warm white; flash in bright white; its own sweep band).
void drawSatRow(const char *satStr, const char *oldSat, int8_t satDir, int16_t rightX, int16_t y, bool ok) {
    int16_t x = rightX - 6 * (int16_t)strlen(satStr);
    float p = ok ? priceFxProgress() : -1.0f;
    int16_t x1 = x + 6 * (int16_t)strlen(satStr);
    fxSweep.on = p >= 0.0f && currentPriceFx == FX_SWEEP;
    fxSweep.x0 = x; fxSweep.x1 = x1;
    fxSweep.bx = x - 4 + (x1 - x + 8) * fxEase(p < 0 ? 0 : p);
    drawFxText(satStr, oldSat, x, y, SAT_RGB[0], SAT_RGB[1], SAT_RGB[2], satDir, p, SAT_FLASH_RGB);
    fxSweep.on = false;
}

// Draws a tImage bitmap with every lit pixel overridden to a single
// color, ignoring whatever's baked into the source data — unlike
// dma_display->drawRGBBitmap() (the standard Adafruit_GFX call, used
// elsewhere for bitmaps that should keep their own real colors, like the
// logo), which has no tint option. Needed for the 万 glyph specifically,
// so it can follow the price direction color instead of always rendering
// in its own fixed baked-in color regardless of what the price is doing.
void drawBitmapTinted(const tImage *img, int16_t x, int16_t y, uint16_t tintColor) {
    for (int16_t row = 0; row < img->height; row++) {
        for (int16_t col = 0; col < img->width; col++) {
            if (img->data[row * img->width + col] != 0) {
                dma_display->drawPixel(x + col, y + row, tintColor);
            }
        }
    }
}

// Per-pixel version of the block bevel (same math as drawBlockchainIcon's
// block() helper), used only by the new-block animation below. Unlike
// that helper — which uses bulk fillRect/drawFastHLine calls, fine for
// the static display since it's redrawn at most a few times a minute —
// this draws pixel-by-pixel so it can clip anything left of clipMinX,
// needed while the existing chain visually exits through the row's left
// edge during the animation's chain-shift phase. The performance
// difference is irrelevant here — this runs ~20 times over one ~1.3s
// animation, not during steady-state display.
void drawBlockImageClipped(int16_t bx, int16_t y0, uint8_t r, uint8_t g, uint8_t b, int16_t clipMinX) {
    uint8_t hiR = (uint8_t)(r + (255 - r) * 0.55f + 0.5f);
    uint8_t hiG = (uint8_t)(g + (255 - g) * 0.55f + 0.5f);
    uint8_t hiB = (uint8_t)(b + (255 - b) * 0.55f + 0.5f);
    uint8_t shR = (uint8_t)(r * 0.5f + 0.5f);
    uint8_t shG = (uint8_t)(g * 0.5f + 0.5f);
    uint8_t shB = (uint8_t)(b * 0.5f + 0.5f);

    auto px = [&](int16_t x, int16_t y, uint8_t pr, uint8_t pg, uint8_t pb) {
        if (x < clipMinX) return;
        dma_display->drawPixel(x, y, dma_display->color565(pr, pg, pb));
    };

    for (int16_t yy = 1; yy < 6; yy++)
        for (int16_t xx = 1; xx < 6; xx++)
            px(bx + xx, y0 + yy, r, g, b);
    for (int16_t i = 0; i < 7; i++) {
        px(bx + i, y0,     hiR, hiG, hiB);
        px(bx,     y0 + i, hiR, hiG, hiB);
    }
    for (int16_t i = 0; i < 7; i++) {
        px(bx + i, y0 + 6, shR, shG, shB);
        px(bx + 6, y0 + i, shR, shG, shB);
    }
}

// N pixels in a horizontal row, left-clipped at clipMinX — covers both
// the chain stub and the inter-block links during the animation, which
// are otherwise identical (a short run of dots that may partially exit
// through the row's left edge).
void drawClippedDots(int16_t x, int16_t y, uint8_t r, uint8_t g, uint8_t b, int16_t clipMinX, uint8_t count = 3) {
    uint16_t color = dma_display->color565(r, g, b);
    for (int16_t i = 0; i < count; i++) {
        if (x + i >= clipMinX) dma_display->drawPixel(x + i, y, color);
    }
}

// Decimal digit count of a uint32_t — mirrors the simulator's
// measureTextWidth(n.toString()) used to keep the Sat/$1 row pinned to
// the old block value's width until the new number actually starts
// sliding in.
uint8_t numDigits(uint32_t n) {
    if (n == 0) return 1;
    uint8_t count = 0;
    while (n > 0) { count++; n /= 10; }
    return count;
}

// Draws a sequence of characters, each trimmed to its own tight column
// range within a shared source bitmap, scaled uniformly, and packed with
// a small scaled gap between them — lets more of the available width go
// to actual content instead of each glyph's built-in cell padding.
// tintColor: optional — when nonzero, overrides every lit pixel to this
// single color instead of the bitmap's own baked-in color (0 = no tint,
// draw the source data as-is).
// target: where to draw — the panel itself, or the off-screen titleCanvas
// when the title is going to be beveled (see drawTitleBeveled()).
void drawScaledRegions(Adafruit_GFX &target, const tImage *img, const GlyphRegion *regions, uint8_t count,
                        int16_t startX, int16_t startY, float scale, uint8_t gapPx,
                        uint16_t tintColor = 0) {
    int16_t cx = startX;
    uint16_t dstH = (uint16_t)(img->height * scale + 0.5f);
    int16_t scaledGap = (int16_t)(gapPx * scale + 0.5f);
    for (uint8_t i = 0; i < count; i++) {
        uint8_t srcX = regions[i].x, srcW = regions[i].w;
        uint16_t dstW = (uint16_t)(srcW * scale + 0.5f);
        for (uint16_t row = 0; row < dstH; row++) {
            uint16_t srcRow = (uint16_t)((uint32_t)row * img->height / dstH);
            for (uint16_t col = 0; col < dstW; col++) {
                uint16_t srcCol = srcX + (uint16_t)((uint32_t)col * srcW / dstW);
                uint16_t px = img->data[srcRow * img->width + srcCol];
                if (px != 0) target.drawPixel(cx + col, startY + row, tintColor ? tintColor : px);
            }
        }
        cx += dstW + scaledGap;
    }
}

// ---------- Beveled titles ----------
// Subtle pixel bevel, the same treatment as the beveled logos (and the
// simulator's drawBeveled()): the title is first drawn as a plain on/off
// shape into this off-screen 1-bit canvas, then copied to the panel with
// every lit pixel re-coloured by its neighbours — top or left neighbour
// dark: highlight; bottom or right neighbour dark: shadow; both or neither:
// body colour. Light from the top-left, like the logos and the blockchain
// blocks. The canvas covers the title area (the panel's top 24 rows) at the
// same coordinates as the panel, and is allocated once (288 bytes) rather
// than on every redraw.
GFXcanvas1 titleCanvas(PANEL_RES_X, 24);

void drawTitleBeveled() {
    const uint16_t HI     = dma_display->color565(255, 255, 255);
    const uint16_t BODY   = dma_display->color565(215, 215, 215);
    const uint16_t SHADOW = dma_display->color565(150, 150, 150);
    for (int16_t y = 0; y < titleCanvas.height(); y++) {
        for (int16_t x = 0; x < titleCanvas.width(); x++) {
            if (!titleCanvas.getPixel(x, y)) continue;   // getPixel() returns false outside the canvas too
            bool litEdge    = !titleCanvas.getPixel(x, y - 1) || !titleCanvas.getPixel(x - 1, y);
            bool shadowEdge = !titleCanvas.getPixel(x, y + 1) || !titleCanvas.getPixel(x + 1, y);
            uint16_t c = (litEdge && !shadowEdge) ? HI : (shadowEdge && !litEdge) ? SHADOW : BODY;
            dma_display->drawPixel(x, y, c);
        }
    }
}

// ---------- Globals ----------
Preferences preferences;
WebServer server(80);   // served from loop() via server.handleClient() — see serveWeb()
DNSServer dnsServer;    // setup mode only: answers every name with BlockGrid's own address

// One persistent client per host, rather than either (a) one shared client
// reused across *different* hosts, which causes intermittent failures from
// stale cross-host TLS session state, or (b) a fresh client created and
// destroyed on every request, which repeatedly allocates and frees each
// client's large TLS buffers and fragments the ESP32 heap over a long run
// (connections increasingly refused, clustering together). Each
// of these only ever talks to one host, so there's no cross-host reuse
// risk, and reusing the same object across calls means its buffers get
// reused rather than freed and reallocated every single cycle.
WiFiClientSecure coingeckoClient;
WiFiClientSecure mempoolClient;
WiFiClient nodePlainClient;   // My node over plain http:// (https:// uses mempoolClient)

const char* PARAM_INPUT_1 = "ssid";
const char* PARAM_INPUT_2 = "password";

unsigned long lastUpdateMs = 0;      // tracks last data refresh (non-blocking)
unsigned long reconnectStartMs = 0;  // when the current reconnect attempt started
unsigned long reconnectDelayMs = WIFI_RETRY_FIRST_MS;   // how long this attempt gets before a fresh one
unsigned long wifiLostSinceMs = 0;   // when the connection was first seen down (0 = connected)
unsigned long lastOfflineRedrawMs = 0;
String savedSsid, savedPassword;     // the home network, kept for reconnecting
unsigned long apLastSavedTryMs = 0, apSavedTryStartMs = 0;
bool apTryingSaved = false;

enum class DeviceState { RUNNING, RECONNECTING, AP_MODE };
DeviceState currentState = DeviceState::RUNNING;

String apPassword;      // generated per-device in startAccessPoint()
bool otaInitialized = false;

enum class ButtonEvent : uint8_t { NONE, SHORT_PRESS, LONG_PRESS };

// Reusable debounced-button poll — each physical button gets its own
// instance so the two don't share or clobber each other's timing state.
struct DebouncedButton {
    uint8_t pin;
    int lastRawState = HIGH;
    int stableState  = HIGH;
    unsigned long lastDebounceTimeMs = 0;
    unsigned long pressStartMs = 0;
    bool longFired = false;

    explicit DebouncedButton(uint8_t p) : pin(p) {}

    // Call every loop() pass (pull-up wiring: pressed = LOW).
    // LONG_PRESS fires once, the instant the hold crosses the threshold —
    // while still held, not on release, so it feels immediate rather than
    // delayed. SHORT_PRESS fires on release, only if LONG_PRESS didn't
    // already fire for that same press (so a press never produces both).
    ButtonEvent poll() {
        int reading = digitalRead(pin);
        ButtonEvent event = ButtonEvent::NONE;

        if (reading != lastRawState) {
            lastDebounceTimeMs = millis();
        }

        if ((millis() - lastDebounceTimeMs) > BUTTON_DEBOUNCE_MS) {
            if (reading != stableState) {
                stableState = reading;
                if (stableState == LOW) {
                    pressStartMs = millis();
                    longFired = false;
                } else if (!longFired) {
                    event = ButtonEvent::SHORT_PRESS;
                }
            } else if (stableState == LOW && !longFired &&
                       (millis() - pressStartMs) >= LONG_PRESS_THRESHOLD_MS) {
                longFired = true;
                event = ButtonEvent::LONG_PRESS;
            }
        }

        lastRawState = reading;
        return event;
    }
};

DebouncedButton brandButton{BUTTON_BRAND_PIN};
DebouncedButton currencyButton{BUTTON_CURRENCY_PIN};

// Tracks an in-progress both-buttons-held reboot gesture — see
// handleButtons() and REBOOT_HOLD_MS above.
bool rebootHoldActive = false;
unsigned long rebootHoldStartMs = 0;

// ---------- Web control page: hand-off from the web handlers to loop() ----------
// The handlers only record what was asked for; processWebCommands() carries
// it out, the same way a button press is handled. -1 = nothing pending.
// (The handlers run inside loop() itself, so the atomics aren't strictly
// needed, but they cost nothing and keep this safe if the server ever
// moves to its own task.)
std::atomic<int32_t> webPendingBrightness{-1};
std::atomic<int32_t> webPendingBrand{-1};
std::atomic<int32_t> webPendingCurrency{-1};
std::atomic<int32_t> webPendingPriceFx{-1};
std::atomic<int32_t> webPendingPriceSrc{-1};
std::atomic<int32_t> webPendingBlockSrc{-1};
std::atomic<int32_t> webPendingInterval{-1};
std::atomic<int32_t> webPendingDisplay{-1};   // 0 = turn the display off, 1 = back on
std::atomic<uint32_t> webRebootRequestedMs{0};   // 0 = no reboot requested
volatile bool bootComplete = false;              // splash finished, normal display running
esp_timer_handle_t splashTimer = nullptr;       // draws splash frames during the first fetch (see splashTimerCallback())
volatile bool splashCallbackBusy = false;
bool webServerStarted = false;

// Brightness changes from the web slider arrive several times a second while
// it's being dragged; the new level is applied at once but only written to
// flash after it has stopped changing for this long.
#define BRIGHTNESS_SAVE_DELAY_MS 2000UL
unsigned long brightnessSaveDueMs = 0;           // 0 = nothing waiting to be saved

// ---------- Data source settings (web control page, saved in Preferences) ----------
// Where the price and block height come from, and how often they're checked.
// Changed on the web control page ("Data sources"); the names below are what
// the page shows, in the same order.
enum PriceSource : uint8_t { PRICE_SRC_AUTO, PRICE_SRC_COINGECKO, PRICE_SRC_BACKUP, NUM_PRICE_SRC };
const char *const priceSrcNames[NUM_PRICE_SRC] = { "Auto", "CoinGecko", "Backup only" };
//   Auto        CoinGecko, and if it fails mempool.space prices + Coinbase 24h change
//   CoinGecko   CoinGecko only, no backup
//   Backup only mempool.space prices + Coinbase 24h change; CoinGecko never asked
enum BlockSource : uint8_t { BLOCK_SRC_AUTO, BLOCK_SRC_MEMPOOL, BLOCK_SRC_BLOCKSTREAM, BLOCK_SRC_NODE, NUM_BLOCK_SRC };
const char *const blockSrcNames[NUM_BLOCK_SRC] = { "Auto", "mempool.space", "blockstream.info", "My node" };
//   Auto        mempool.space, then blockstream.info if it fails
//   My node     your own node's mempool (nodeUrl), then as Auto if it fails
// My node's address, e.g. "https://192.168.1.50:50194" — the mempool app as
// you'd open it in a browser. Saved in Preferences ("nodeUrl"); set on the
// web page. Empty = not set (My node then works as Auto).
#define NODE_URL_MAX 120
String nodeUrl;
int8_t nodeOk = -1;                // last answer from the node: 1 yes, 0 no, -1 not asked yet
char nodeErr[40] = "";             // why it didn't answer, for the web page
char blockErr[40] = "";            // why the last block-height request failed (fetchBlockHeightFrom())
const char *lastBlockFrom = "";    // where the block height shown came from (shown on the web page)
String webPendingNodeUrl;          // from the web page, applied in processWebCommands()
std::atomic<bool> webPendingNodeUrlSet{false};

// Tidies a node address from the page ("https://host:port/", ".../api" or
// the full ".../api/blocks/tip/height" → "https://host:port"). Returns false
// if it doesn't look like one.
bool normaliseNodeUrl(String &u) {
    u.trim();
    int api = u.indexOf("/api/", 8);   // 8: past "https://"
    if (api > 0) u.remove(api);
    while (u.endsWith("/")) u.remove(u.length() - 1);
    if (u.endsWith("/api")) u.remove(u.length() - 4);
    if (u.isEmpty()) return true;   // cleared
    if (!(u.startsWith("http://") || u.startsWith("https://")) || u.length() > NODE_URL_MAX) return false;
    for (size_t i = 0; i < u.length(); i++) {
        char c = u[i];
        if (c <= ' ' || c == '"' || c == '\\' || c == '<' || c == '>' || c == '\'') return false;
    }
    return true;
}
#define NUM_INTERVALS 5
const uint16_t intervalSecs[NUM_INTERVALS] = { 30, 60, 120, 300, 600 };
const char *const intervalNames[NUM_INTERVALS] = { "30 s", "1 min", "2 min", "5 min", "10 min" };
#define DEFAULT_INTERVAL_INDEX 3   // 5 minutes
uint8_t currentPriceSrc = PRICE_SRC_AUTO;
uint8_t currentBlockSrc = BLOCK_SRC_AUTO;
uint8_t currentInterval = DEFAULT_INTERVAL_INDEX;
const char *lastPriceFrom = "";    // where the price shown came from (shown on the web page)

// The "Check every" setting applies to the price only. The block height is
// always checked every minute, so a new block shows up promptly whatever
// the price setting. loop() checks whichever is due.
#define BLOCK_CHECK_MS 60000UL
unsigned long lastPriceCheckMs = 0;   // 0 = never checked (due now)
unsigned long lastBlockCheckMs = 0;
unsigned long updateIntervalMs() { return intervalSecs[currentInterval] * 1000UL; }
// Timed from the start of the previous check, so a slow fetch doesn't push
// the next one later and later.
bool priceCheckDue() { return lastPriceCheckMs == 0 || millis() - lastPriceCheckMs >= updateIntervalMs(); }
bool blockCheckDue() { return lastBlockCheckMs == 0 || millis() - lastBlockCheckMs >= BLOCK_CHECK_MS; }
// Cached prices are shown until they're this old, then the row shows ERR:
// 5 minutes, or three missed checks when checking less often than that.
// (The block height always uses 5 minutes — see BLOCK_STALE_AFTER_MS.)
unsigned long dataStaleAfterMs() {
    unsigned long three = 3UL * updateIntervalMs();
    return three > 5UL * 60UL * 1000UL ? three : 5UL * 60UL * 1000UL;
}

// Brand/currency cycling state — independent indices, independent Preferences keys
uint8_t currentBrandIndex    = 0;   // index into brandProfiles[]
uint8_t currentCurrencyIndex = 0;   // index into currencyOptions[]
uint8_t currentBrightness    = 60;  // 0-255, matches setBrightness8()'s range
// Display off: the panel goes fully dark (brightness 0) while everything
// else keeps running — prices and blocks update, the web page works — so it
// shows current data the moment it's turned back on. Set from the web page's
// switch, or by pressing − at the lowest brightness; any button press, the
// switch, or moving the brightness slider turns it back on. Not saved: a
// restart always comes back on.
bool displayOff = false;

// Cached from the last successful fetch, so a button press can redraw
// immediately without waiting for the next data-refresh cycle.
float    lastPrice        = 0.0f;   // BTC/USD
float    lastSatoshis     = 0.0f;   // sats per USD
float    lastPriceJPY     = 0.0f;   // BTC/JPY
float    lastSatsPer100Yen = 0.0f;  // sats per ¥100
uint32_t lastBlockHeight  = 0;
float    lastChangePctUSD = NAN;    // 24h % change, cached like the other last* values (NAN = unknown)
float    lastChangePctJPY = NAN;
float    lastPriceEUR     = 0.0f;   // BTC/EUR
float    lastSatsPerEUR   = 0.0f;   // sats per €1
float    lastChangePctEUR = NAN;

// The last* values above only ever hold the last SUCCESSFUL fetch — a
// failed fetch leaves them alone. The screen keeps showing them and only
// switches to ERR once they're older than DATA_STALE_AFTER_MS (see
// currentSnapshot()). Price and block height are tracked separately since
// they come from different servers and fail independently.
#define DATA_STALE_AFTER_MS (dataStaleAfterMs())   // prices: 5 min, or 3 missed checks if checking less often
#define BLOCK_STALE_AFTER_MS (5UL * 60UL * 1000UL)   // block height: ~5 missed checks
bool          havePriceData   = false;
bool          haveBlockData   = false;
unsigned long lastGoodPriceMs = 0;
unsigned long lastGoodBlockMs = 0;

// Per-device network name: "blockgrid-" + the last 4 hex digits of the
// Wi-Fi MAC address (e.g. MAC 24:6F:28:AB:3F:2A -> "blockgrid-3f2a"), so
// several BlockGrids on one network each get their own .local address,
// router entry and OTA port. Fixed for the life of the chip — nothing to
// configure. Used for mDNS (blockgrid-xxxx.local), the DHCP hostname and
// ArduinoOTA.
String getDeviceHostname() {
    uint64_t mac = ESP.getEfuseMac();   // byte 0 = first MAC byte, byte 5 = last
    char buf[16];
    snprintf(buf, sizeof(buf), "blockgrid-%02x%02x",
             (unsigned)((mac >> 32) & 0xFF), (unsigned)((mac >> 40) & 0xFF));
    return String(buf);
}

// Per-device password derived from the chip's MAC. Used for both the setup
// AP and OTA updates — one password to know, shown on the matrix during
// setup, rather than a second hardcoded secret to manage separately.
String getDevicePassword() {
    uint64_t chipId = ESP.getEfuseMac();
    char buf[9];
    snprintf(buf, sizeof(buf), "%08X", (uint32_t)(chipId & 0xFFFFFFFFUL));
    return String(buf);
}

// ---------- Web server handlers ----------
// Setup mode: someone is using the setup page, so the saved network isn't
// retried for another AP_RETRY_SAVED_MS — a retry moves the setup network's
// channel and would knock their phone off it.
void noteSetupPageInUse() {
    if (currentState == DeviceState::AP_MODE && !apTryingSaved) apLastSavedTryMs = millis();
}

// The Wi-Fi setup page (setup mode's "/"). It fetches the nearby networks
// from /api/wifi/scan and posts the choice to /save. Kept identical to the
// simulator's SETUP_PAGE_HTML. Sent straight from flash.
static const char SETUP_PAGE[] PROGMEM = R"HTMLPAGE(<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1">
<title>BlockGrid Setup</title>
<style>
  /* Orange BlockGrid theme, same as the web control page and the
     simulator. In setup mode the phone is on BlockGrid's own network with
     no internet, so the Orbitron / Share Tech Mono fonts usually can't load
     and the built-in monospace font is used instead. */
  @import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Orbitron:wght@500;700&display=swap');
  :root {
    --bg: #0b0b0c; --card: #131315; --accent: #ff9f1c; --accent-bright: #ffc15e; --accent-dim: #5c3a0c;
    --text: #d6d6d6; --text-dim: #8a8a8f; --border: #232327; --danger: #ff4a4a; --field: #1a1a1d;
    --mono: 'Share Tech Mono', ui-monospace, 'SFMono-Regular', Menlo, Consolas, monospace;
    --head: 'Orbitron', var(--mono);
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; min-height: 100vh; display: flex; align-items: center; justify-content: center;
    background: var(--bg); color: var(--text);
    font-family: var(--mono);
    padding: 24px 16px;
  }
  input, button, select { font-family: inherit; }
  .card {
    width: 100%; max-width: 380px; background: var(--card); border: 1px solid var(--border);
    border-radius: 16px; padding: 28px 24px; box-shadow: 0 8px 32px rgba(0,0,0,.5);
  }
  .logo { text-align: center; font-family: var(--head); font-size: 20px; font-weight: 700; letter-spacing: .22em; padding-left: .22em; text-transform: uppercase; margin-bottom: 8px; }
  .logo span { color: var(--accent); }
  .subtitle { text-align: center; color: var(--text-dim); font-size: 13px; margin-bottom: 24px; }
  label { display: block; font-family: var(--head); font-size: 10px; font-weight: 500; letter-spacing: .24em; text-transform: uppercase; color: var(--text-dim); margin: 18px 0 8px; }
  .field { position: relative; }
  .netrow { display: flex; gap: 8px; }
  input[type="text"], input[type="password"], select {
    width: 100%; min-width: 0; height: 48px; background: var(--field); border: 1px solid var(--border);
    border-radius: 10px; color: var(--text); font-size: 16px; padding: 0 14px;
  }
  select { flex: 1; padding: 0 10px; }
  input[type="password"], .field input[type="text"] { padding-right: 64px; }
  input:focus, select:focus { outline: none; border-color: var(--accent-dim); }
  .toggle-pw {
    position: absolute; right: 4px; top: 4px; height: 40px; padding: 0 12px;
    background: none; border: none; color: var(--text-dim); font-size: 13px; cursor: pointer;
  }
  .scan {
    flex: none; height: 48px; padding: 0 14px; border-radius: 10px; border: 1px solid var(--border);
    background: var(--field); color: var(--text); font-size: 14px; cursor: pointer;
  }
  .scan:disabled { color: var(--text-dim); cursor: default; }
  button[type="submit"] {
    width: 100%; height: 50px; margin-top: 24px; background: var(--accent); color: #111;
    border: none; border-radius: 10px; font-size: 16px; font-weight: 700; cursor: pointer;
  }
  button[type="submit"]:disabled { background: var(--accent-dim); color: #444; }
  .error { color: var(--danger); font-size: 13px; text-align: center; margin-top: 12px; min-height: 16px; }
  .hint { color: var(--text-dim); font-size: 12px; text-align: center; margin-top: 16px; line-height: 1.5; }
  .small { color: var(--text-dim); font-size: 11.5px; margin-top: 8px; line-height: 1.5; }
</style>
</head>
<body>
<div class="card">
  <div class="logo">Block<span>Grid</span></div>
  <div class="subtitle">Connect to your WiFi network</div>

  <form id="setupForm" action="/save" method="post" onsubmit="return onSubmit()">
    <label for="nets">Nearby networks</label>
    <div class="netrow">
      <select id="nets"><option value="">Looking for networks…</option></select>
      <button type="button" class="scan" id="rescan" onclick="scan(true)">Scan</button>
    </div>
    <div class="small">Not listed (a hidden network)? Type its name below.</div>

    <label for="ssid">Network name</label>
    <input type="text" name="ssid" id="ssid" maxlength="32" placeholder="Network name" autocomplete="off" autocapitalize="off" spellcheck="false">

    <label for="password">Password</label>
    <div class="field">
      <input type="password" name="password" id="password" maxlength="64" placeholder="Network password" autocomplete="off">
      <button type="button" class="toggle-pw" onclick="togglePw()">Show</button>
    </div>

    <div class="error" id="formError"></div>
    <button type="submit" id="submitBtn">Connect</button>
  </form>
  <div class="hint">BlockGrid will restart and connect to this network once submitted.</div>
</div>

<script>
const $ = id => document.getElementById(id);

// Nearby networks: BlockGrid scans once as its setup network starts, before
// any phone has joined, and again only when Scan is pressed — a scan can
// knock the phone off the setup network for a moment. Names are put into
// the page as text (never as HTML), so any network name is shown safely.
let scanTimer = null;
async function scan(fresh) {
  clearTimeout(scanTimer);
  if (fresh) { $('rescan').disabled = true; $('rescan').textContent = '…'; }
  let r = null;
  try {
    const res = await fetch('/api/wifi/scan', fresh ? { method: 'POST' } : { cache: 'no-store' });
    if (res.ok) r = await res.json();
  } catch (e) {}
  if (!r) { $('rescan').disabled = false; $('rescan').textContent = 'Scan'; scanTimer = setTimeout(() => scan(false), 3000); return; }
  fillNets(r);
  if (r.scanning) scanTimer = setTimeout(() => scan(false), 1000);
}

function fillNets(r) {
  const sel = $('nets'), keep = sel.value, nets = r.networks || [];
  sel.textContent = '';
  const add = (value, text) => { const o = document.createElement('option'); o.value = value; o.textContent = text; sel.appendChild(o); return o; };
  add('', r.scanning ? 'Scanning…' : nets.length ? 'Choose a network (' + nets.length + ')' : 'No networks found');
  nets.forEach(n => { add(n.ssid, n.ssid + '  ' + n.rssi + ' dBm' + (n.open ? ' · open' : '')).dataset.open = n.open ? '1' : ''; });
  sel.value = keep;
  if (sel.value !== keep) sel.value = '';
  $('rescan').disabled = !!r.scanning;
  $('rescan').textContent = r.scanning ? '…' : 'Scan';
}

$('nets').onchange = () => {
  const o = $('nets').selectedOptions[0];
  if (!o || !o.value) return;
  $('ssid').value = o.value;
  $('password').placeholder = o.dataset.open ? 'No password needed' : 'Network password';
  if (o.dataset.open) $('password').value = '';   // a password makes the join fail on an open network
  $('formError').textContent = '';
  if (!o.dataset.open) $('password').focus();
};

function togglePw() {
  const pw = $('password');
  const btn = document.querySelector('.toggle-pw');
  const isHidden = pw.type === 'password';
  pw.type = isHidden ? 'text' : 'password';
  btn.textContent = isHidden ? 'Hide' : 'Show';
}

function onSubmit() {
  const ssid = $('ssid').value, pw = $('password').value;
  const err = $('formError');
  if (!ssid.trim()) { err.textContent = 'Choose or enter a network name.'; return false; }
  if (new TextEncoder().encode(ssid).length > 32) { err.textContent = 'That network name is too long.'; return false; }
  if (pw.length && (pw.length < 8 || pw.length > 64)) { err.textContent = 'Wi-Fi passwords are 8 to 63 characters.'; return false; }
  $('submitBtn').disabled = true;
  $('submitBtn').textContent = 'Connecting…';
  return true;
}

scan(false);
</script>
</body>
</html>
)HTMLPAGE";

void handleRoot() {
    noteSetupPageInUse();
    server.send_P(200, "text/html", SETUP_PAGE, sizeof(SETUP_PAGE) - 1);
}

void handleSaveWifi() {
    // Setup mode only — on your Wi-Fi, the control page's Wi-Fi card (which
    // tries a network before saving it) is the way to change networks.
    if (currentState != DeviceState::AP_MODE) { server.send(404, "text/plain", "Not found"); return; }
    if (!server.hasArg(PARAM_INPUT_1) || !server.hasArg(PARAM_INPUT_2)) {
        server.send(400, "text/plain", "Missing SSID or password.");
        return;
    }
    String ssid     = server.arg(PARAM_INPUT_1);
    String password = server.arg(PARAM_INPUT_2);
    const char *err = wifiCredentialsError(ssid, password);
    if (err) { server.send(400, "text/plain", err); return; }

    preferences.putString("ssid", ssid);
    preferences.putString("password", password);

    server.send(200, "text/html", R"HTMLPAGE(<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>BlockGrid Setup</title>
<style>
  body {
    margin: 0; min-height: 100vh; display: flex; align-items: center; justify-content: center;
    background: #0b0b0c; color: #d6d6d6;
    font-family: 'Share Tech Mono', ui-monospace, 'SFMono-Regular', Menlo, Consolas, monospace;
  }
  .card { text-align: center; padding: 32px; }
  .spinner {
    width: 36px; height: 36px; margin: 0 auto 20px; border: 3px solid #232327;
    border-top-color: #ff9f1c; border-radius: 50%; animation: spin .8s linear infinite;
  }
  @keyframes spin { to { transform: rotate(360deg); } }
  h2 { margin: 0 0 8px; font-size: 18px; }
  p { color: #8a8a8f; font-size: 14px; margin: 0; }
</style>
</head>
<body>
<div class="card">
  <div class="spinner"></div>
  <h2>Saved</h2>
  <p>BlockGrid is restarting and connecting to your network…</p>
</div>
</body>
</html>)HTMLPAGE");
    delay(1000);
    ESP.restart();
}

// ---------- Web control page ----------
// Served at http://blockgrid-xxxx.local/ (see getDeviceHostname()) or the device's IP once BlockGrid is on
// your Wi-Fi. Kept identical to the simulator's CONTROL_PAGE_HTML — design
// changes there first, then copy the page here. Stored in flash and sent
// straight from it, never copied into RAM. The page talks to these small
// endpoints:
//   GET  /api/status   JSON snapshot (see buildStatusJson())
//   POST /api/set      brightness=1..255, brand=<index>, currency=<index>,
//                      effect=<index>, priceSource=<index>, blockSource=<index>,
//                      interval=<index>, display=0|1 (display off / on),
//                      nodeUrl=<My node's address>
//                      (form-encoded; any combination) — returns the status
//   POST /api/reboot   reboots half a second after replying
//   GET/POST /api/wifi/scan, POST /api/wifi   the Wi-Fi card (see "Nearby
//                      Wi-Fi networks" and "Moving to another Wi-Fi network")
// It checks /api/status every 5 s while open (not at all in a background
// tab), and sends one change at a time — a few hundred bytes each, well
// clear of the memory the HTTPS price fetch needs.
static const char CONTROL_PAGE[] PROGMEM = R"HTMLPAGE(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="theme-color" content="#0b0b0c">
<title>BlockGrid</title>
<style>
  /* Orange BlockGrid theme — same fonts and colours as the simulator:
     Orbitron for the BLOCKGRID wordmark and section labels, Share Tech Mono
     for everything else. The fonts load from Google Fonts when the phone has
     internet; without it the page falls back to the built-in monospace font
     and still works. Price up/down stays green/red, since it means something. */
  @import url('https://fonts.googleapis.com/css2?family=Share+Tech+Mono&family=Orbitron:wght@500;700&display=swap');
  :root {
    --bg: #0b0b0c; --card: #131315; --accent: #ff9f1c; --accent-bright: #ffc15e; --accent-dim: #5c3a0c;
    --text: #d6d6d6; --text-dim: #8a8a8f; --border: #232327; --danger: #ff4a4a; --field: #1a1a1d;
    --up: #00ff46; --down: #ff4a4a;
    --mono: 'Share Tech Mono', ui-monospace, 'SFMono-Regular', Menlo, Consolas, monospace;
    --head: 'Orbitron', var(--mono);
  }
  * { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  button { font-family: inherit; }
  body {
    margin: 0; min-height: 100vh; background: var(--bg); color: var(--text);
    font-family: var(--mono);
    padding: 24px 16px 32px;
  }
  main { max-width: 380px; margin: 0 auto; }
  header { text-align: center; margin-bottom: 20px; }
  .logo { font-family: var(--head); font-size: 20px; font-weight: 700; letter-spacing: .22em; text-transform: uppercase; margin-bottom: 8px; padding-left: .22em; }
  .logo span { color: var(--accent); }
  .subtitle { color: var(--text-dim); font-size: 13px; display: flex; justify-content: center; align-items: center; gap: 6px; flex-wrap: wrap; }
  .pill { display: inline-flex; align-items: center; gap: 6px; }
  .pill i { width: 8px; height: 8px; border-radius: 50%; background: var(--text-dim); }
  .pill.live i { background: var(--up); box-shadow: 0 0 6px var(--up); }
  .pill.warn i { background: #ffb020; }
  .pill.off i { background: var(--danger); }
  .devname:not(:empty)::before { content: '·'; margin-right: 6px; }
  .card {
    background: var(--card); border: 1px solid var(--border); border-radius: 16px;
    padding: 20px 20px 22px; margin-bottom: 14px; box-shadow: 0 8px 32px rgba(0,0,0,.5);
  }
  h2 { font-family: var(--head); font-size: 10px; font-weight: 500; letter-spacing: .24em; text-transform: uppercase; color: var(--text-dim); margin: 0 0 12px; }
  h2 + * { margin-top: 0; }
  .now { text-align: center; }
  .now .block { font-size: 13px; color: var(--text-dim); }
  .now .block b { color: var(--accent); font-size: 18px; font-weight: 700; margin-left: 6px; font-variant-numeric: tabular-nums; }
  .now .price { font-size: 34px; font-weight: 700; margin: 8px 0 4px; font-variant-numeric: tabular-nums; }
  .now .price.up { color: var(--up); } .now .price.down { color: var(--down); }
  .now .row { display: flex; justify-content: center; gap: 14px; font-size: 14px; flex-wrap: wrap; }
  .chg.up { color: var(--up); } .chg.down { color: var(--down); }
  .sats { color: #ffe4be; }
  .muted { color: var(--text-dim); font-size: 12px; margin-top: 12px; }
  .slider { display: flex; align-items: center; gap: 10px; }
  .step {
    width: 48px; height: 48px; flex: none; border-radius: 10px; border: 1px solid var(--border);
    background: var(--field); color: var(--text); font-size: 22px; line-height: 1; cursor: pointer;
  }
  input[type=range] { -webkit-appearance: none; appearance: none; flex: 1; min-width: 0; height: 6px; border-radius: 3px; background: var(--border); outline: none; }
  input[type=range]::-webkit-slider-thumb { -webkit-appearance: none; width: 26px; height: 26px; border-radius: 50%; background: var(--accent); border: 0; }
  input[type=range]::-moz-range-thumb { width: 26px; height: 26px; border-radius: 50%; background: var(--accent); border: 0; }
  h2 .val { float: right; font-family: var(--mono); font-size: 14px; letter-spacing: 0; color: var(--accent); }
  .seg { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 8px; }
  .seg button {
    min-height: 48px; border-radius: 10px; border: 1px solid var(--border); background: var(--field);
    color: var(--text); font-size: 15px; cursor: pointer; padding: 6px 10px; font-family: inherit;
  }
  .seg button.on { background: var(--accent); border-color: var(--accent); color: #111; font-weight: 700; }
  .stats { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin-bottom: 18px; }
  .stats div { font-size: 13px; color: var(--text-dim); }
  .stats b { display: block; color: var(--text); font-size: 15px; font-weight: 600; margin-top: 3px; font-variant-numeric: tabular-nums; }
  .reboot {
    width: 100%; height: 50px; border-radius: 10px; border: 1px solid var(--danger); background: none;
    color: var(--danger); font-size: 16px; font-weight: 700; cursor: pointer; font-family: inherit;
  }
  .reboot.confirm { background: var(--danger); color: #111; }
  button:disabled, input:disabled, select:disabled { opacity: .4; cursor: default; }
  .banner { display: none; text-align: center; color: var(--text-dim); font-size: 12px; margin: -6px 0 14px; line-height: 1.5; }
  .banner.show { display: block; }
  .sub { font-size: 12px; color: var(--text-dim); margin: 16px 0 8px; }
  .sub:first-of-type { margin-top: 0; }
  #intervals { grid-template-columns: repeat(5, minmax(0, 1fr)); gap: 6px; }
  #intervals button { padding: 6px 2px; font-size: 14px; }
  #blockSrcs button, #priceSrcs button { font-size: 14px; }
  .note { font-size: 11.5px; color: var(--text-dim); margin-top: 10px; line-height: 1.5; }
  .power {
    width: 100%; height: 48px; margin-top: 14px; border-radius: 10px; border: 1px solid var(--border);
    background: var(--field); color: var(--text); font-size: 15px; cursor: pointer; font-family: inherit;
  }
  .power.off { background: var(--accent); border-color: var(--accent); color: #111; font-weight: 700; }
  .power[hidden] { display: none; }
  .slider.dim { opacity: .45; }
  .now .fig { min-width: 0; }
  .now .lbl { display: none; }

  /* Wi-Fi card: the network in use, and a form for moving to another one. */
  .wifi-now { font-size: 13px; color: var(--text-dim); }
  .wifi-now b { display: block; color: var(--text); font-size: 15px; font-weight: 600; margin-top: 3px; overflow-wrap: anywhere; }
  #wifiForm { margin-top: 18px; }
  #wifiForm[hidden], #wifiOpen[hidden], .wmsg[hidden], #nodeBox[hidden] { display: none; }
  .netrow { display: flex; gap: 8px; }
  select, .txt {
    width: 100%; min-width: 0; height: 48px; background: var(--field); border: 1px solid var(--border);
    border-radius: 10px; color: var(--text); font-size: 16px; padding: 0 12px; font-family: inherit;
  }
  select { flex: 1; }
  select:focus, .txt:focus { outline: none; border-color: var(--accent-dim); }
  .field { position: relative; }
  .field .txt { padding-right: 64px; }
  .toggle-pw { position: absolute; right: 4px; top: 4px; height: 40px; padding: 0 12px; background: none; border: none; color: var(--text-dim); font-size: 13px; cursor: pointer; font-family: inherit; }
  .power.scan { width: auto; flex: none; margin: 0; padding: 0 14px; font-size: 14px; }
  .btn2 { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-top: 18px; }
  .btn2 .power { margin: 0; }
  .join { height: 48px; border-radius: 10px; border: 1px solid var(--accent); background: var(--accent); color: #111; font-size: 15px; font-weight: 700; cursor: pointer; font-family: inherit; }
  .wmsg { font-size: 12.5px; margin-top: 12px; line-height: 1.5; color: var(--text-dim); }
  .wmsg.bad { color: var(--danger); } .wmsg.good { color: var(--up); }

  /* Mouse and keyboard: hover highlight and a visible focus ring. */
  button:focus-visible, input:focus-visible, select:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
  @media (hover: hover) and (pointer: fine) {
    .seg button:not(.on):not(:disabled):hover, .step:not(:disabled):hover, .power:not(.off):not(:disabled):hover { border-color: var(--accent-dim); background: #222226; }
    .power.off:not(:disabled):hover { background: var(--accent-bright); border-color: var(--accent-bright); }
    .seg button.on:not(:disabled):hover { background: var(--accent-bright); border-color: var(--accent-bright); }
    .reboot:not(:disabled):not(.confirm):hover { background: rgba(255,74,74,.08); }
    .join:not(:disabled):hover { background: var(--accent-bright); border-color: var(--accent-bright); }
    select:not(:disabled), .toggle-pw { cursor: pointer; }
    input[type=range]:not(:disabled) { cursor: pointer; }
  }

  /* Wider screens: header on one line and a wide "now" strip. From 1000 px
     the settings split into two columns — display settings on the left,
     data sources and device on the right. Phones keep the layout above. */
  @media (min-width: 700px) {
    body { padding: 36px 32px 48px; }
    main { max-width: 640px; }
    header { display: flex; align-items: center; justify-content: space-between; text-align: left; margin-bottom: 22px; }
    .logo { font-size: 24px; margin: 0; padding-left: 0; }
    .subtitle { font-size: 14px; }
    .banner { text-align: left; margin: -8px 0 16px; font-size: 13px; }
    .card { padding: 24px 26px 26px; margin-bottom: 18px; }
    .now { display: grid; grid-template-columns: 1.5fr 1fr; align-items: end; gap: 12px 32px; text-align: left; padding: 26px 30px; }
    .now .lbl { display: block; font-family: var(--head); font-size: 10px; letter-spacing: .24em; text-transform: uppercase; color: var(--text-dim); margin-bottom: 10px; }
    .now .prc { order: -1; }
    .now .price { font-size: 52px; margin: 0; line-height: 1; }
    .now .row { justify-content: flex-start; gap: 20px; font-size: 16px; margin-top: 12px; }
    .now .block { font-size: 0; }
    .now .block b { display: block; font-size: 36px; margin: 0; line-height: 1; }
    .now .blk, .now .blk .lbl { text-align: right; }
    .now .muted { grid-column: 1 / -1; margin-top: 4px; padding-top: 14px; border-top: 1px solid var(--border); font-size: 13px; }
    .seg { grid-template-columns: repeat(auto-fit, minmax(96px, 1fr)); }
    .seg button { min-height: 42px; }
    #blockSrcs button, #priceSrcs button { padding: 6px 2px; font-size: 13px; }
    .step { width: 42px; height: 42px; }
    .power { height: 42px; }
    select, .txt, .join { height: 42px; }
    .toggle-pw { top: 1px; }
    .reboot { height: 44px; }
  }
  @media (min-width: 1000px) {
    main { max-width: 1040px; }
    .now .price { font-size: 56px; }
    .now .block b { font-size: 40px; }
    .cols { display: grid; grid-template-columns: 1fr 1fr; gap: 0 18px; align-items: start; }
  }
</style>
</head>
<body>
<main>
  <header>
    <div class="logo">Block<span>Grid</span></div>
    <div class="subtitle"><span class="pill" id="pill"><i></i><span id="pillText">Connecting…</span></span><span class="devname" id="devName"></span></div>
  </header>
  <div class="banner" id="banner"></div>

  <section class="card now">
    <div class="fig blk"><div class="lbl">Block height</div><div class="block">Block<b id="block">–</b></div></div>
    <div class="fig prc"><div class="lbl">Bitcoin price</div><div class="price" id="price">–</div>
      <div class="row"><span class="chg" id="chg"></span><span class="sats" id="sats"></span></div></div>
    <div class="muted" id="updated"></div>
  </section>

  <div class="cols">
  <div class="col">

  <section class="card">
    <h2>Brightness <span class="val" id="briVal">–</span></h2>
    <div class="slider">
      <button class="step" id="dn" aria-label="Dimmer">−</button>
      <input type="range" id="bri" min="0" max="200" aria-label="Brightness">
      <button class="step" id="upb" aria-label="Brighter">+</button>
    </div>
    <button class="power" id="power" hidden>Turn display off</button>
  </section>

  <section class="card">
    <h2>Theme</h2>
    <div class="seg" id="brands"></div>
  </section>

  <section class="card">
    <h2>Currency</h2>
    <div class="seg" id="currencies"></div>
  </section>

  <section class="card">
    <h2>Price change effect</h2>
    <div class="seg" id="effects"></div>
  </section>

  </div>
  <div class="col">
  <section class="card">
    <h2>Data sources</h2>
    <div class="sub">Price</div>
    <div class="seg" id="priceSrcs"></div>
    <div class="sub">Block height</div>
    <div class="seg" id="blockSrcs"></div>
    <div id="nodeBox" hidden>
      <div class="sub">Your node’s mempool address <span style="opacity:.7">(as you’d open it in a browser)</span></div>
      <div class="netrow"><input class="txt" type="url" id="nodeUrl" maxlength="120" placeholder="https:&#47;&#47;192.168.1.50:50194" autocomplete="off" autocapitalize="off" spellcheck="false" aria-label="Node address"><button class="power scan" id="nodeSave">Save</button></div>
      <div class="wmsg" id="nodeMsg"></div>
    </div>
    <div class="sub">Check price every <span style="opacity:.7">(blocks: every minute)</span></div>
    <div class="seg" id="intervals"></div>
    <div class="note" id="srcNote"></div>
  </section>

  <section class="card">
    <h2>Wi-Fi</h2>
    <div class="wifi-now">Connected to<b id="ssidNow">–</b></div>
    <div class="wmsg" id="wifiMsg" hidden></div>
    <button class="power" id="wifiOpen">Change network</button>
    <div id="wifiForm" hidden>
      <div class="sub">Nearby networks</div>
      <div class="netrow"><select id="nets" aria-label="Nearby networks"><option value="">Scanning…</option></select><button class="power scan" id="rescan">Scan again</button></div>
      <div class="sub">Network name <span style="opacity:.7">(type it for a hidden network)</span></div>
      <input class="txt" type="text" id="wssid" maxlength="32" placeholder="Network name" autocomplete="off" autocapitalize="off" spellcheck="false" aria-label="Network name">
      <div class="sub">Password</div>
      <div class="field"><input class="txt" type="password" id="wpass" maxlength="64" placeholder="Network password" autocomplete="off" aria-label="Password"><button class="toggle-pw" id="wshow" type="button">Show</button></div>
      <div class="btn2"><button class="power" id="wifiCancel">Cancel</button><button class="join" id="wifiJoin">Join</button></div>
      <div class="note">BlockGrid tries the new network before saving it, and is off this page for up to 30 s meanwhile. If it can’t connect, it comes back to this network.</div>
    </div>
  </section>

  <section class="card">
    <h2>Device</h2>
    <div class="stats">
      <div>Wi-Fi signal<b id="rssi">–</b></div>
      <div>Uptime<b id="uptime">–</b></div>
      <div>Free memory<b id="heap">–</b></div>
      <div>Largest block<b id="largest">–</b></div>
    </div>
    <button class="reboot" id="reboot">Reboot</button>
  </section>
  </div>
  </div>
</main>

<script>
const $ = id => document.getElementById(id);
let rebootAt = 0;
let S = null, busy = false, dragging = false, rebooting = false, confirmTimer = null, briTimer = null;
// Brightness 1–255 on a curve: the slider (0–200) and −/+ take small steps
// at the dim end, where the eye notices each one most, and bigger ones at
// the bright end. LEVELS is the device's BRIGHTNESS_LEVELS (its buttons).
const LEVELS = [1, 2, 3, 4, 5, 6, 8, 10, 13, 17, 22, 28, 36, 46, 60, 77, 100, 128, 165, 210, 255];
const posToBri = p => Math.round(1 + 254 * Math.pow(p / 200, 2.2));
const briToPos = b => Math.round(200 * Math.pow(Math.max(0, b - 1) / 254, 1 / 2.2));

const fmt = n => Math.round(n).toLocaleString('en-US');
function ago(s) { return s < 5 ? 'just now' : s < 90 ? s + ' s ago' : Math.round(s / 60) + ' min ago'; }
function dur(s) { const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return d ? d + 'd ' + h + 'h' : h ? h + 'h ' + m + 'm' : m + 'm'; }
function signal(r) { return r + ' dBm · ' + (r > -60 ? 'good' : r > -72 ? 'fair' : 'weak'); }

// What each price source setting does (same order as the device's list).
const SRC_NOTES = [
  'Auto: CoinGecko first. If it fails, prices come from mempool.space and the 24h change from Coinbase (the euro and yen % are estimated from the dollar %).',
  'CoinGecko only: no backup. If CoinGecko refuses, the last price stays up until it answers again.',
  'Backup only: prices from mempool.space, 24h change from Coinbase — CoinGecko is never asked.'
];

function pill(cls, text) { $('pill').className = 'pill ' + cls; $('pillText').textContent = text; }
function banner(text) { $('banner').textContent = text || ''; $('banner').classList.toggle('show', !!text); }

function render(s) {
  S = s;
  // Which BlockGrid this is, when there's more than one on the network.
  if (s.name && $('devName').textContent !== s.name + '.local') {
    $('devName').textContent = s.name + '.local';
    document.title = 'BlockGrid · ' + s.name;
  }
  const ready = s.mode === 'normal' || s.mode === 'error';
  if (s.mode === 'starting') { pill('warn', 'Starting up'); banner('BlockGrid is starting up — controls will be ready in a moment.'); }
  else if (s.mode === 'error') { pill('off', 'No data'); banner('BlockGrid can’t reach the price service right now.'); }
  else if (s.stale)          { pill('warn', 'Data stale'); banner('Price data couldn’t be refreshed — showing the last values received.'); }
  else                       { pill('live', 'Live'); banner(''); }

  // Block height and price come from different services and can fail
  // independently; each shows a dash when the device has nothing current.
  const jpy = s.currency === 'JPY', blockOk = s.block > 0, priceOk = s.price > 0;
  const sym = jpy ? '¥' : s.currency === 'EUR' ? '€' : '$';
  $('block').textContent = blockOk ? fmt(s.block) : '–';
  $('price').textContent = priceOk ? sym + fmt(s.price) : '–';
  const up = s.change === null || s.change >= 0;
  $('price').className = 'price ' + (priceOk ? (up ? 'up' : 'down') : '');
  $('chg').className = 'chg ' + (up ? 'up' : 'down');
  $('chg').textContent = priceOk && s.change !== null ? (up ? '▲ ' : '▼ ') + Math.abs(s.change).toFixed(2) + '% 24h' : '';
  $('sats').textContent = priceOk ? fmt(s.sats) + (jpy ? ' sat / ¥100' : ' sat / ' + sym + '1') : '';
  $('updated').textContent = blockOk || priceOk ? 'Updated ' + ago(s.updatedAgo) + (s.priceFrom ? ' · ' + s.priceFrom : '') : '';

  // Display off: the panel is dark (brightness 0) but everything else keeps
  // running. Moving the slider or pressing −/+ turns it back on.
  const off = s.display === false;
  if (!dragging) { $('bri').value = briToPos(s.brightness); $('briVal').textContent = off ? 'Off' : s.brightness; }
  $('power').hidden = s.display === undefined;   // firmware without the switch
  $('power').textContent = off ? 'Turn display on' : 'Turn display off';
  $('power').classList.toggle('off', off);
  document.querySelector('.slider').classList.toggle('dim', off);
  segs('brands', s.brands, s.brand, i => send('brand=' + i));
  segs('currencies', s.currencies, s.currencies.indexOf(s.currency), i => send('currency=' + i));
  if (s.effects) segs('effects', s.effects, s.effect, i => send('effect=' + i));
  if (s.priceSrcs) {
    segs('priceSrcs', s.priceSrcs, s.priceSrc, i => send('priceSource=' + i));
    segs('blockSrcs', s.blockSrcs, s.blockSrc, i => send('blockSource=' + i));
    segs('intervals', s.intervals, s.interval, i => send('interval=' + i));
    $('srcNote').textContent = SRC_NOTES[s.priceSrc] || '';
    nodeRender(s);
  }

  $('rssi').textContent = signal(s.rssi);
  $('uptime').textContent = dur(s.uptime);
  $('heap').textContent = Math.round(s.heap / 1024) + ' KB';
  $('largest').textContent = Math.round(s.largest / 1024) + ' KB';

  if (s.ssid !== undefined) $('ssidNow').textContent = s.ssid || '–';
  // Back from trying another network: it either failed (and BlockGrid
  // returned here) or worked and this page reached it on the new one.
  if (joining && Date.now() - joining.at > 3000) {
    if (s.wifiFail && s.wifiFail === joining.ssid) { wifiMsg('Couldn’t join “' + joining.ssid + '”. Check the name and password — BlockGrid is still on “' + s.ssid + '”.', 'bad'); joining = null; }
    else if (s.ssid === joining.ssid) { wifiMsg('Now on “' + s.ssid + '”.', 'good'); joining = null; }
  }

  document.querySelectorAll('button, input, select').forEach(el => { el.disabled = !ready; });
}

// Rebuild the button row only when the list changes; otherwise just move the highlight.
function segs(id, names, sel, onPick) {
  const box = $(id);
  if (box.dataset.names !== names.join('|')) {
    box.dataset.names = names.join('|'); box.innerHTML = '';
    names.forEach((n, i) => { const b = document.createElement('button'); b.textContent = n; b.onclick = () => onPick(i); box.appendChild(b); });
  }
  [...box.children].forEach((b, i) => b.classList.toggle('on', i === sel));
}

async function poll() {
  try {
    const r = await fetch('/api/status', { cache: 'no-store' });
    if (!r.ok) throw 0;
    const s = await r.json();
    if (rebooting && Date.now() - rebootAt < 3000) return;   // still answering from before the restart
    if (rebooting && s.mode !== 'starting') rebooting = false;
    render(s);
  } catch (e) {
    if (joining) {
      pill('warn', 'Switching Wi-Fi'); banner(joinText());
      document.querySelectorAll('button, input, select').forEach(el => el.disabled = true);
      return;
    }
    pill('off', rebooting ? 'Rebooting' : 'Offline');
    banner(rebooting ? 'Rebooting — this page will reconnect by itself.' : 'Can’t reach BlockGrid. Is it powered on and on this Wi-Fi network?');
    document.querySelectorAll('button, input, select').forEach(el => el.disabled = true);
  }
}

// One request at a time; if another change comes in meanwhile, only the
// latest is sent once the current one finishes (keeps the device's load tiny).
let pending = null;
async function send(body) {
  if (busy) { pending = body; return; }
  busy = true;
  let ok = false;
  try {
    const r = await fetch('/api/set', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body });
    if (r.ok) { ok = true; const s = await r.json(); if (!pending) render(s); }
  } catch (e) {}
  busy = false;
  if (pending) { const b = pending; pending = null; send(b); }
  else if (!ok) poll();
}

// Brightness: live while dragging (throttled), final value on release.
$('bri').addEventListener('input', e => {
  const b = posToBri(+e.target.value);
  dragging = true; $('briVal').textContent = b;
  clearTimeout(briTimer); briTimer = setTimeout(() => send('brightness=' + b), 150);
});
$('bri').addEventListener('change', e => { clearTimeout(briTimer); dragging = false; send('brightness=' + posToBri(+e.target.value)); });
function stepBri(dir) {   // to the next level up or down
  if (!S) return;
  const b = S.brightness;
  const next = dir > 0 ? (LEVELS.find(l => l > b) || 255) : (LEVELS.filter(l => l < b).pop() || 1);
  send('brightness=' + next);
}
$('dn').onclick = () => stepBri(-1);
$('power').onclick = () => { if (S) send('display=' + (S.display === false ? 1 : 0)); };
$('upb').onclick = () => stepBri(1);

// Reboot: tap once to arm, tap again within 4 s to confirm ("click" with a mouse).
const TAP = matchMedia('(hover: hover) and (pointer: fine)').matches ? 'Click' : 'Tap';
$('reboot').onclick = async () => {
  const b = $('reboot');
  if (!b.classList.contains('confirm')) {
    b.classList.add('confirm'); b.textContent = TAP + ' again to reboot';
    confirmTimer = setTimeout(() => { b.classList.remove('confirm'); b.textContent = 'Reboot'; }, 4000);
    return;
  }
  clearTimeout(confirmTimer); b.classList.remove('confirm'); b.textContent = 'Reboot';
  rebooting = true; rebootAt = Date.now();
  pill('off', 'Rebooting'); banner('Rebooting — this page will reconnect by itself.');
  document.querySelectorAll('button, input, select').forEach(el => el.disabled = true);
  try { await fetch('/api/reboot', { method: 'POST' }); } catch (e) {}
};

// My node: the block height from the mempool app on your own node (e.g. a
// Start9 on this network). BlockGrid asks it first and falls back to
// mempool.space and blockstream.info whenever it doesn't answer.
let nodeDirty = false;
function nodeRender(s) {
  const mine = s.blockSrcs[s.blockSrc] === 'My node';
  $('nodeBox').hidden = !mine;
  if (!mine) return;
  if (!nodeDirty && document.activeElement !== $('nodeUrl')) $('nodeUrl').value = s.nodeUrl || '';
  const m = $('nodeMsg');
  if (!s.nodeUrl) { m.className = 'wmsg'; m.textContent = 'Enter your node’s address. Until then the block height comes from mempool.space and blockstream.info.'; }
  else if (s.nodeOk === true) { m.className = 'wmsg good'; m.textContent = 'Block height from your node.'; }
  else if (s.nodeOk === false) { m.className = 'wmsg bad'; m.textContent = 'Your node didn’t answer' + (s.nodeErr ? ' (' + s.nodeErr + ')' : '') + (s.blockFrom ? ' — using ' + s.blockFrom + ' meanwhile' : '') + '. Check the address, and that the node is on and reachable from this network.'; }
  else { m.className = 'wmsg'; m.textContent = 'Checking your node…'; }
}
$('nodeUrl').addEventListener('input', () => { nodeDirty = true; });
$('nodeSave').onclick = () => {
  let v = $('nodeUrl').value.trim().replace(/\/+$/, '').replace(/(:\/\/[^\/]+)\/api(\/.*)?$/, '$1');   // the full API address works too
  if (!/^https?:\/\/[^\s\x22\x27<>\\]+$/.test(v) || v.length > 120) {   // \x22 \x27: quote marks (kept out of the sketch's raw string, which confuses the Arduino IDE)
    $('nodeMsg').className = 'wmsg bad';
    $('nodeMsg').textContent = 'Enter the address starting with https:\/\/ (or http:\/\/), e.g. https:\/\/192.168.1.50:50194';
    return;
  }
  $('nodeUrl').value = v; nodeDirty = false;
  send('nodeUrl=' + encodeURIComponent(v));
};
$('nodeUrl').addEventListener('keydown', e => { if (e.key === 'Enter') $('nodeSave').click(); });

// Wi-Fi: move BlockGrid to another network. The list comes from a scan the
// device runs when the form opens (and on "Scan again"); names are only ever
// put into the page as text. BlockGrid tries the new network first and keeps
// it only if it connects — otherwise it returns to this one and says so.
let joining = null, scanTimer = null;   // joining = { ssid, at } while it tries
function wifiMsg(text, cls) { const m = $('wifiMsg'); m.textContent = text || ''; m.className = 'wmsg ' + (cls || ''); m.hidden = !text; }
function joinText() {
  const name = S && S.name ? S.name + '.local' : 'BlockGrid';
  return Date.now() - joining.at < 90000
    ? 'BlockGrid is trying “' + joining.ssid + '”. If it connects, it restarts on that network; if not, it comes back here within about 30 s.'
    : 'BlockGrid hasn’t come back to this network, so it’s probably on “' + joining.ssid + '” now. Join that network and open ' + name + '.';
}
function showWifiForm(show) {
  $('wifiForm').hidden = !show; $('wifiOpen').hidden = show;
  clearTimeout(scanTimer);
  if (show) { $('wssid').value = ''; $('wpass').value = ''; wifiMsg(''); fillNets({ scanning: true, networks: [] }); scan(true); }
}
async function scan(fresh) {
  clearTimeout(scanTimer);
  let r = null;
  try {
    const res = await fetch('/api/wifi/scan', fresh ? { method: 'POST' } : { cache: 'no-store' });
    if (res.ok) r = await res.json();
  } catch (e) {}
  if (!r) { $('rescan').textContent = 'Scan again'; return; }
  fillNets(r);
  if (r.scanning && !$('wifiForm').hidden) scanTimer = setTimeout(() => scan(false), 1000);
}
function fillNets(r) {
  const sel = $('nets'), keep = sel.value, nets = r.networks || [];
  sel.textContent = '';
  const add = (value, text) => { const o = document.createElement('option'); o.value = value; o.textContent = text; sel.appendChild(o); return o; };
  add('', r.scanning ? 'Scanning…' : nets.length ? 'Choose a network (' + nets.length + ')' : 'No networks found');
  nets.forEach(n => { add(n.ssid, n.ssid + '  ' + n.rssi + ' dBm' + (n.open ? ' · open' : '')).dataset.open = n.open ? '1' : ''; });
  sel.value = keep;
  if (sel.value !== keep) sel.value = '';
  $('rescan').textContent = r.scanning ? 'Scanning…' : 'Scan again';
  $('rescan').dataset.busy = r.scanning ? '1' : '';
}
$('nets').onchange = () => {
  const o = $('nets').selectedOptions[0];
  if (!o || !o.value) return;
  $('wssid').value = o.value;
  $('wpass').placeholder = o.dataset.open ? 'No password needed' : 'Network password';
  if (o.dataset.open) $('wpass').value = '';   // a password makes the join fail on an open network
  wifiMsg('');
  if (!o.dataset.open) $('wpass').focus();
};
$('rescan').onclick = () => { if (!$('rescan').dataset.busy) scan(true); };
$('wshow').onclick = () => { const p = $('wpass'), hid = p.type === 'password'; p.type = hid ? 'text' : 'password'; $('wshow').textContent = hid ? 'Hide' : 'Show'; };
$('wifiOpen').onclick = () => showWifiForm(true);
$('wifiCancel').onclick = () => showWifiForm(false);
$('wifiJoin').onclick = async () => {
  const ssid = $('wssid').value, pw = $('wpass').value;
  let err = '';
  if (!ssid.trim()) err = 'Choose or enter a network name.';
  else if (new TextEncoder().encode(ssid).length > 32) err = 'That network name is too long.';
  else if (pw.length && (pw.length < 8 || pw.length > 64)) err = 'Wi-Fi passwords are 8 to 63 characters.';
  if (err) { wifiMsg(err, 'bad'); return; }
  $('wifiJoin').disabled = true;
  let r = null;
  try {
    const res = await fetch('/api/wifi', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'ssid=' + encodeURIComponent(ssid) + '&password=' + encodeURIComponent(pw) });
    r = await res.json();
  } catch (e) {}
  $('wifiJoin').disabled = false;
  if (!r || !r.ok) { wifiMsg(r && r.error ? r.error : 'BlockGrid didn’t answer — try again.', 'bad'); return; }
  joining = { ssid, at: Date.now() };
  showWifiForm(false);
  wifiMsg('Trying “' + ssid + '”…', '');
  pill('warn', 'Switching Wi-Fi'); banner(joinText());
};

// Check in every 5 s while the page is open (every 2 s while rebooting or
// switching networks); nothing at all while the tab is in the background.
async function loop() { if (!document.hidden) await poll(); setTimeout(loop, rebooting || joining ? 2000 : 5000); }
loop();
document.addEventListener('visibilitychange', () => { if (!document.hidden) poll(); });
</script>
</body>
</html>
)HTMLPAGE";

// ---------- Nearby Wi-Fi networks (setup page and control page) ----------
// The strongest WIFI_SCAN_MAX networks from the last scan, one entry per
// name (hidden networks left out), strongest first. Scans run in the
// background (WiFi.scanNetworks(true)) and pollWifiScan() collects the
// result, so the display and web page keep going meanwhile. In setup mode
// the first scan runs before the setup network starts (see
// startAccessPoint()): a scan hops channels, which can drop a phone off the
// setup network for a moment, so later scans happen only when asked for.
//   GET  /api/wifi/scan   {"scanning":bool,"networks":[{"ssid","rssi","open"}]}
//   POST /api/wifi/scan   starts a scan, replies as GET
#define WIFI_SCAN_MAX 20
#define WIFI_SCAN_TIMEOUT_MS 15000UL
struct NearbyNetwork { char ssid[33]; int8_t rssi; bool open; };
NearbyNetwork nearby[WIFI_SCAN_MAX];
uint8_t nearbyCount = 0;
bool scanRunning = false;
unsigned long scanStartMs = 0;
std::atomic<bool> webPendingScan{false};

// Copies the finished scan's results into nearby[] and frees them.
void collectScanResults(int16_t n) {
    nearbyCount = 0;
    for (int16_t i = 0; i < n; i++) {
        String name = WiFi.SSID(i);
        if (name.isEmpty() || name.length() > 32) continue;   // hidden network
        int8_t rssi = (int8_t)WiFi.RSSI(i);
        bool open = WiFi.encryptionType(i) == WIFI_AUTH_OPEN;
        uint8_t k = 0;
        while (k < nearbyCount && strcmp(nearby[k].ssid, name.c_str()) != 0) k++;
        if (k < nearbyCount) {   // same name from another access point: keep the stronger
            if (rssi > nearby[k].rssi) { nearby[k].rssi = rssi; nearby[k].open = open; }
            continue;
        }
        if (nearbyCount == WIFI_SCAN_MAX) {   // full: replace the weakest if this one is stronger
            uint8_t w = 0;
            for (uint8_t j = 1; j < nearbyCount; j++) if (nearby[j].rssi < nearby[w].rssi) w = j;
            if (rssi <= nearby[w].rssi) continue;
            k = w;
        } else {
            k = nearbyCount++;
        }
        strlcpy(nearby[k].ssid, name.c_str(), sizeof(nearby[k].ssid));
        nearby[k].rssi = rssi;
        nearby[k].open = open;
    }
    WiFi.scanDelete();
    for (uint8_t i = 1; i < nearbyCount; i++) {   // strongest first
        NearbyNetwork t = nearby[i];
        int8_t j = i - 1;
        while (j >= 0 && nearby[j].rssi < t.rssi) { nearby[j + 1] = nearby[j]; j--; }
        nearby[j + 1] = t;
    }
    Serial.printf("Wi-Fi scan: %u networks nearby\n", nearbyCount);
}

void startWifiScan() {
    if (scanRunning) return;
    // A scan can't run while the setup mode's retry of the saved network is
    // connecting; the scan wins, and the retry waits its usual 2 minutes.
    if (currentState == DeviceState::AP_MODE && apTryingSaved) {
        WiFi.disconnect();
        apTryingSaved = false;
        apLastSavedTryMs = millis();
    }
    int16_t r = WiFi.scanNetworks(true, false);   // in the background; hidden networks not listed
    scanRunning = (r == WIFI_SCAN_RUNNING);
    scanStartMs = millis();
    if (!scanRunning) Serial.println("Wi-Fi scan couldn't start");
}

// Called from loop(): starts a scan the web page asked for, and collects
// the results when it finishes.
void pollWifiScan() {
    if (webPendingScan.exchange(false)) startWifiScan();
    if (!scanRunning) return;
    int16_t n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) {
        if (millis() - scanStartMs > WIFI_SCAN_TIMEOUT_MS) { WiFi.scanDelete(); scanRunning = false; }
        return;
    }
    scanRunning = false;
    if (n >= 0) collectScanResults(n);
    else WiFi.scanDelete();
}

// Writes s into out as the inside of a JSON string: " and \ escaped,
// control characters as \u00XX. Network names can contain anything.
void jsonEscape(const char *s, char *out, size_t outSize) {
    size_t n = 0;
    for (; *s && n + 7 < outSize; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) n += snprintf(out + n, outSize - n, "\\u%04x", c);
        else out[n++] = (char)c;
    }
    out[n] = '\0';
}

void handleApiWifiScan() {
    noteSetupPageInUse();
    if (server.method() == HTTP_POST) webPendingScan.store(true);
    bool scanning = scanRunning || webPendingScan.load();
    String json;
    json.reserve(40 + nearbyCount * 60);
    json = scanning ? "{\"scanning\":true,\"networks\":[" : "{\"scanning\":false,\"networks\":[";
    char esc[200], item[260];
    for (uint8_t i = 0; i < nearbyCount; i++) {
        jsonEscape(nearby[i].ssid, esc, sizeof(esc));
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                 i ? "," : "", esc, (int)nearby[i].rssi, nearby[i].open ? "true" : "false");
        json += item;
    }
    json += "]}";
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
}

// ---------- Moving to another Wi-Fi network (web control page) ----------
// POST /api/wifi  ssid=, password= (empty for an open network)
//   {"ok":true} once accepted, or {"ok":false,"error":"..."}.
// The handler only records the request; processWifiJoin() tries it from
// loop() half a second later, once the page has its reply. The new network
// is kept only if BlockGrid connects to it within WIFI_TIMEOUT_MS: then it's
// saved and BlockGrid restarts on it. If not, BlockGrid goes back to the
// saved network (through the usual reconnect in loop()) and the status JSON
// names the network that failed ("wifiFail") so the page can say so.
std::atomic<uint32_t> webPendingJoinMs{0};   // when the request came in (0 = none)
String joinSsid, joinPassword;
char wifiFailSsid[33] = "";
unsigned long wifiFailMs = 0;
#define WIFI_FAIL_REPORT_MS (10UL * 60UL * 1000UL)   // how long the page is told about a failed attempt

// Why a network name / password can't be right, or nullptr if they can.
const char *wifiCredentialsError(const String &ssid, const String &pass) {
    if (ssid.length() == 0 || ssid.length() > 32) return "Network names are 1 to 32 characters.";
    if (pass.length() != 0 && (pass.length() < 8 || pass.length() > 64)) return "Wi-Fi passwords are 8 to 63 characters.";
    return nullptr;
}

void sendWifiError(int code, const char *msg) {
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    server.send(code, "application/json", buf);
}

void handleApiWifi() {
    if (!apiAvailable()) return;
    if (!bootComplete) { sendWifiError(503, "BlockGrid is still starting up. Try again in a moment."); return; }
    if (webPendingJoinMs.load() != 0) { sendWifiError(409, "Already trying a network."); return; }
    String ssid = server.arg("ssid"), pass = server.arg("password");
    const char *err = wifiCredentialsError(ssid, pass);
    if (err) { sendWifiError(400, err); return; }
    joinSsid = ssid;
    joinPassword = pass;
    server.send(200, "application/json", "{\"ok\":true}");
    webPendingJoinMs.store(millis() | 1UL);   // | 1 so it's never 0 (= "none")
}

void processWifiJoin() {
    uint32_t at = webPendingJoinMs.load();
    if (at == 0 || millis() - at < 500) return;
    Serial.printf("Trying the Wi-Fi network \"%s\" (asked for on the web page)...\n", joinSsid.c_str());
    if (scanRunning) { WiFi.scanDelete(); scanRunning = false; }
    wifiFailSsid[0] = '\0';
    WiFi.disconnect();
    delay(100);
    WiFi.begin(joinSsid.c_str(), joinPassword.c_str());
    WiFi.setSleep(false);
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) delay(50);

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("Joined \"%s\" (%s) — saving it\n", joinSsid.c_str(), WiFi.localIP().toString().c_str());
        preferences.putString("ssid", joinSsid);
        preferences.putString("password", joinPassword);
        rebootNow("New Wi-Fi network saved");   // a clean start on the new network (mDNS, OTA, connections)
    }

    Serial.printf("Couldn't join \"%s\" — going back to \"%s\"\n", joinSsid.c_str(), savedSsid.c_str());
    strlcpy(wifiFailSsid, joinSsid.c_str(), sizeof(wifiFailSsid));
    wifiFailMs = millis();
    joinSsid = "";
    joinPassword = "";
    webPendingJoinMs.store(0);
    WiFi.disconnect();
    delay(100);
    WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
    WiFi.setSleep(false);
    // loop()'s reconnect handling takes it from here.
    unsigned long now = millis();
    wifiLostSinceMs = now;
    reconnectStartMs = now;
    reconnectDelayMs = WIFI_RETRY_FIRST_MS;
    lastOfflineRedrawMs = now;
    currentState = DeviceState::RECONNECTING;
}

// Captive portal: in setup mode every name the phone looks up leads here
// (see dnsServer), so its "is there internet?" check (/generate_204,
// /hotspot-detect.html, ...) arrives as an unknown page. Redirecting it to
// the setup page makes the phone open that page by itself.
void handleNotFound() {
    if (currentState == DeviceState::AP_MODE) {
        server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
        server.send(302, "text/plain", "");
        return;
    }
    server.send(404, "text/plain", "Not found");
}

// Writes the device's current state as JSON into buf. Pending web changes
// that loop() hasn't applied yet are reported as already applied, so the
// page shows the new value straight away rather than bouncing back to the
// old one until its next check.
void buildStatusJson(char *buf, size_t bufSize) {
    unsigned long now = millis();
    PriceSnapshot snap = currentSnapshot();

    int32_t pb = webPendingBrightness.load();
    int32_t pbr = webPendingBrand.load();
    int32_t pc = webPendingCurrency.load();
    int32_t pfx = webPendingPriceFx.load();
    uint8_t fxIdx = pfx >= 0 ? (uint8_t)pfx : currentPriceFx;
    uint8_t brightness = pb >= 0 ? (uint8_t)pb : currentBrightness;
    uint8_t brandIdx   = pbr >= 0 ? (uint8_t)pbr : currentBrandIndex;
    uint8_t currIdx    = pc >= 0 ? (uint8_t)pc : currentCurrencyIndex;
    Currency cur = currencyOptions[currIdx];
    bool jpy = cur == Currency::JPY, eur = cur == Currency::EUR;

    // "error" = the panel is showing ERR for both rows (nothing current at
    // all); "stale" = still showing cached values, but the last successful
    // fetch is more than two refresh cycles old.
    const char *mode = !bootComplete ? "starting"
                     : (snap.priceUSD <= 0.0f && snap.blockHeight == 0) ? "error" : "normal";
    bool stale = (havePriceData && now - lastGoodPriceMs > 2 * updateIntervalMs()) ||
                 (haveBlockData && now - lastGoodBlockMs > 2 * BLOCK_CHECK_MS);
    unsigned long lastGood = havePriceData ? lastGoodPriceMs : lastGoodBlockMs;
    unsigned long updatedAgo = (havePriceData || haveBlockData) ? (now - lastGood) / 1000UL : 0;

    float price  = jpy ? snap.priceJPY : eur ? snap.priceEUR : snap.priceUSD;
    float sats   = jpy ? snap.satsPer100Yen : eur ? snap.satsPerEUR : snap.satsPerUSD;
    float change = jpy ? snap.changePctJPY : eur ? snap.changePctEUR : snap.changePctUSD;
    char changeStr[12];
    if (isnan(change)) strcpy(changeStr, "null");
    else snprintf(changeStr, sizeof(changeStr), "%.2f", change);

    // Brand and currency name lists
    char brands[160] = "", currencies[40] = "";
    size_t n = 0;
    for (uint8_t i = 0; i < NUM_BRAND_PROFILES && n < sizeof(brands); i++)
        n += snprintf(brands + n, sizeof(brands) - n, "%s\"%s\"", i ? "," : "", brandNames[i]);
    n = 0;
    for (uint8_t i = 0; i < NUM_CURRENCY_OPTIONS && n < sizeof(currencies); i++)
        n += snprintf(currencies + n, sizeof(currencies) - n, "%s\"%s\"", i ? "," : "",
                      currencyCode(currencyOptions[i]));
    char priceSrcs[64] = "", blockSrcs[80] = "", intervals[64] = "";
    n = 0;
    for (uint8_t i = 0; i < NUM_PRICE_SRC && n < sizeof(priceSrcs); i++)
        n += snprintf(priceSrcs + n, sizeof(priceSrcs) - n, "%s\"%s\"", i ? "," : "", priceSrcNames[i]);
    n = 0;
    for (uint8_t i = 0; i < NUM_BLOCK_SRC && n < sizeof(blockSrcs); i++)
        n += snprintf(blockSrcs + n, sizeof(blockSrcs) - n, "%s\"%s\"", i ? "," : "", blockSrcNames[i]);
    n = 0;
    for (uint8_t i = 0; i < NUM_INTERVALS && n < sizeof(intervals); i++)
        n += snprintf(intervals + n, sizeof(intervals) - n, "%s\"%s\"", i ? "," : "", intervalNames[i]);
    int32_t pps = webPendingPriceSrc.load(), pbs = webPendingBlockSrc.load(), piv = webPendingInterval.load();
    int32_t pd = webPendingDisplay.load();
    // A brightness change from the page turns the display back on (see processWebCommands()).
    bool dispOn = pd >= 0 ? pd == 1 : (pb >= 0 ? true : !displayOff);
    char effects[48] = "";
    n = 0;
    for (uint8_t i = 0; i < NUM_PRICE_FX && n < sizeof(effects); i++)
        n += snprintf(effects + n, sizeof(effects) - n, "%s\"%s\"", i ? "," : "", priceFxNames[i]);

    // The network in use, and one the page asked for that didn't work.
    char ssidEsc[200], failEsc[200], nodeEsc[2 * NODE_URL_MAX + 8];
    bool nodeUrlPending = webPendingNodeUrlSet.load();
    jsonEscape(nodeUrlPending ? webPendingNodeUrl.c_str() : nodeUrl.c_str(), nodeEsc, sizeof(nodeEsc));
    jsonEscape(WiFi.SSID().c_str(), ssidEsc, sizeof(ssidEsc));
    jsonEscape(wifiFailSsid[0] && now - wifiFailMs < WIFI_FAIL_REPORT_MS ? wifiFailSsid : "", failEsc, sizeof(failEsc));

    snprintf(buf, bufSize,
        "{\"name\":\"%s\",\"mode\":\"%s\",\"display\":%s,\"brightness\":%u,\"brand\":%u,\"brands\":[%s],"
        "\"currency\":\"%s\",\"currencies\":[%s],\"effect\":%u,\"effects\":[%s],"
        "\"block\":%lu,\"price\":%.0f,\"change\":%s,\"sats\":%.0f,"
        "\"updatedAgo\":%lu,\"stale\":%s,\"priceFrom\":\"%s\","
        "\"priceSrc\":%u,\"priceSrcs\":[%s],\"blockSrc\":%u,\"blockSrcs\":[%s],\"interval\":%u,\"intervals\":[%s],"
        "\"rssi\":%d,\"ssid\":\"%s\",\"wifiFail\":\"%s\",\"nodeUrl\":\"%s\",\"nodeOk\":%s,\"nodeErr\":\"%s\",\"blockFrom\":\"%s\",\"heap\":%u,\"largest\":%u,\"uptime\":%lu}",
        getDeviceHostname().c_str(), mode, dispOn ? "true" : "false", brightness, brandIdx, brands,
        currencyCode(cur), currencies, fxIdx, effects,
        (unsigned long)snap.blockHeight, price, changeStr, sats,
        updatedAgo, stale ? "true" : "false", lastPriceFrom,
        pps >= 0 ? (unsigned)pps : currentPriceSrc, priceSrcs, pbs >= 0 ? (unsigned)pbs : currentBlockSrc, blockSrcs,
        piv >= 0 ? (unsigned)piv : currentInterval, intervals,
        (int)WiFi.RSSI(), ssidEsc, failEsc,
        nodeEsc, nodeUrlPending || nodeOk < 0 ? "null" : nodeOk ? "true" : "false", nodeErr, lastBlockFrom,
        (unsigned)ESP.getFreeHeap(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), now / 1000UL);
}

void sendStatus(int code) {
    char buf[1792];
    buildStatusJson(buf, sizeof(buf));
    server.sendHeader("Cache-Control", "no-store");
    server.send(code, "application/json", buf);
}

// "/" serves whichever page fits the mode: the Wi-Fi setup page while in AP
// setup mode, the control page otherwise. The /api routes only answer
// outside AP mode.
void handleRootOrControlPage() {
    if (currentState == DeviceState::AP_MODE) { handleRoot(); return; }
    server.send_P(200, "text/html", CONTROL_PAGE, sizeof(CONTROL_PAGE) - 1);
}

bool apiAvailable() {
    if (currentState != DeviceState::AP_MODE) return true;
    server.send(404, "text/plain", "Not found");
    return false;
}

void handleApiStatus() {
    if (!apiAvailable()) return;
    sendStatus(200);
}

void handleApiSet() {
    if (!apiAvailable()) return;
    // Settings can't be changed during the boot splash — the device doesn't
    // act on its buttons then either.
    if (!bootComplete) { sendStatus(503); return; }

    if (server.hasArg("brightness")) {
        long v = server.arg("brightness").toInt();
        if (v < BRIGHTNESS_MIN) v = BRIGHTNESS_MIN;
        if (v > BRIGHTNESS_MAX) v = BRIGHTNESS_MAX;
        webPendingBrightness.store((int32_t)v);
    }
    if (server.hasArg("brand")) {
        long v = server.arg("brand").toInt();
        if (v >= 0 && v < NUM_BRAND_PROFILES) webPendingBrand.store((int32_t)v);
    }
    if (server.hasArg("currency")) {
        long v = server.arg("currency").toInt();
        if (v >= 0 && v < NUM_CURRENCY_OPTIONS) webPendingCurrency.store((int32_t)v);
    }
    if (server.hasArg("priceSource")) {
        long v = server.arg("priceSource").toInt();
        if (v >= 0 && v < NUM_PRICE_SRC) webPendingPriceSrc.store((int32_t)v);
    }
    if (server.hasArg("nodeUrl")) {
        String u = server.arg("nodeUrl");
        if (normaliseNodeUrl(u)) { webPendingNodeUrl = u; webPendingNodeUrlSet.store(true); }
    }
    if (server.hasArg("blockSource")) {
        long v = server.arg("blockSource").toInt();
        if (v >= 0 && v < NUM_BLOCK_SRC) webPendingBlockSrc.store((int32_t)v);
    }
    if (server.hasArg("interval")) {
        long v = server.arg("interval").toInt();
        if (v >= 0 && v < NUM_INTERVALS) webPendingInterval.store((int32_t)v);
    }
    if (server.hasArg("display")) {
        long v = server.arg("display").toInt();
        if (v == 0 || v == 1) webPendingDisplay.store((int32_t)v);
    }
    if (server.hasArg("effect")) {
        long v = server.arg("effect").toInt();
        if (v >= 0 && v < NUM_PRICE_FX) webPendingPriceFx.store((int32_t)v);
    }
    sendStatus(200);
}

void handleApiReboot() {
    if (!apiAvailable()) return;
    server.send(200, "application/json", "{\"ok\":true}");
    webRebootRequestedMs.store(millis() | 1UL);   // | 1 so it's never 0 (= "none")
}

// Registers every route and starts the server — once per boot, whichever
// mode comes first. The handlers above pick the right behaviour for the
// current mode, so nothing needs re-registering if Wi-Fi is lost and the
// device falls back to AP setup mode.
void startWebServer() {
    if (webServerStarted) return;
    server.on("/", HTTP_GET, handleRootOrControlPage);
    server.on("/save", HTTP_POST, handleSaveWifi);
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/set", HTTP_POST, handleApiSet);
    server.on("/api/reboot", HTTP_POST, handleApiReboot);
    server.on("/api/wifi/scan", HTTP_GET, handleApiWifiScan);    // both modes
    server.on("/api/wifi/scan", HTTP_POST, handleApiWifiScan);
    server.on("/api/wifi", HTTP_POST, handleApiWifi);
    server.onNotFound(handleNotFound);   // setup mode: redirect to the setup page (captive portal)
    server.begin();
    webServerStarted = true;
}

// Answers any waiting web requests. Called from loop() (through
// handleButtons()) and from the places that wait a while, so the page stays
// responsive; while an HTTPS fetch is in progress (a few seconds each
// minute), requests simply wait for it to finish.
void serveWeb() {
    if (webServerStarted) server.handleClient();
}

// Starts the web control page once BlockGrid is on your Wi-Fi.
void startWebControl() {
    startWebServer();
    Serial.printf("Web control page: http://%s.local/  or  http://%s/\n",
                  getDeviceHostname().c_str(), WiFi.localIP().toString().c_str());
}

void startAccessPoint() {
    currentState = DeviceState::AP_MODE;
    setDisplayOff(false, "Wi-Fi setup");   // the setup details are shown on the panel

    // Unique per-device password so the setup network isn't open to anyone
    // nearby. It's shown only on the matrix itself, so only someone standing
    // in front of the device can read it. Also reused for OTA (see setupOTA).
    apPassword = getDevicePassword();

    // List the nearby networks for the setup page now, before the setup
    // network is up: a scan hops channels, which would briefly drop a phone
    // that had already joined. (Stops any connection attempt still running
    // first — a scan can't run alongside one.)
    WiFi.disconnect();
    WiFi.mode(WIFI_AP_STA);
    int16_t found = WiFi.scanNetworks(false, false);
    if (found >= 0) collectScanResults(found);
    else WiFi.scanDelete();

    WiFi.softAP("BlockGrid", apPassword.c_str());
    apLastSavedTryMs = millis();   // first retry of the saved network in AP_RETRY_SAVED_MS
    apTryingSaved = false;
    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(IP);
    Serial.print("AP password: ");
    Serial.println(apPassword);

    // Captive portal: every name looked up on the setup network answers
    // with BlockGrid's address, so the phone's internet check reaches
    // handleNotFound() and the phone opens the setup page by itself.
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", IP);

    // Serves the setup page (see handleRootOrControlPage()); already
    // running if Wi-Fi was lost after the control page started.
    startWebServer();

    // Show setup prompt on display — colors match the boot splash's own
    // palette exactly (200,255,200 bright near-white-green / 0,255,70
    // trail green, see drawSplashFrame() below), rather than the
    // unrelated amber/blue scheme this used before. Bright near-white for
    // values the person actually needs to read correctly (the password,
    // the IP), dimmer trail green for static labels — meaningful, not
    // arbitrary: it's the same "bright = important" language the splash
    // itself uses for the resolved word against the dimmer falling rain.
    dma_display->clearScreen();
    dma_display->setTextSize(1);
    dma_display->setTextColor(dma_display->color565(200, 255, 200));
    dma_display->setCursor(2, 1);
    dma_display->print("WiFi Setup");
    dma_display->setCursor(2, 10);
    dma_display->setTextColor(dma_display->color565(0, 255, 70));
    dma_display->print("SSID: BlockGrid");
    dma_display->setCursor(2, 19);
    dma_display->setTextColor(dma_display->color565(200, 255, 200));
    dma_display->print(IP.toString());
    dma_display->setCursor(2, 28);
    dma_display->setTextColor(dma_display->color565(0, 255, 70));
    dma_display->print("AP pass:");
    dma_display->setCursor(2, 37);
    dma_display->setTextColor(dma_display->color565(200, 255, 200));
    dma_display->print(apPassword);
    dma_display->flipDMABuffer();
}

// In Wi-Fi setup mode with a saved network, try that network again every
// AP_RETRY_SAVED_MS (for WIFI_TIMEOUT_MS). The setup network stays up the
// whole time; when the saved one answers, BlockGrid restarts normally.
void retrySavedNetworkFromSetup() {
    if (savedSsid.isEmpty()) return;   // nothing saved yet: setup is the only way on
    unsigned long now = millis();
    if (!apTryingSaved) {
        if (scanRunning || now - apLastSavedTryMs < AP_RETRY_SAVED_MS) return;
        Serial.printf("Setup mode: trying the saved network \"%s\" again...\n", savedSsid.c_str());
        WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
        apTryingSaved = true;
        apSavedTryStartMs = now;
        return;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Saved network is back — restarting");
        delay(200);
        ESP.restart();
    }
    if (now - apSavedTryStartMs >= WIFI_TIMEOUT_MS) {
        WiFi.disconnect();   // the setup network stays up
        apTryingSaved = false;
        apLastSavedTryMs = millis();
    }
}

void setupOTA() {
    String otaPassword = getDevicePassword();

    String hostname = getDeviceHostname();
    ArduinoOTA.setHostname(hostname.c_str());   // copied by ArduinoOTA, so the local String going away is fine
    ArduinoOTA.setPassword(otaPassword.c_str());

    ArduinoOTA.onStart([]() {
        Serial.println("OTA update starting...");
        setDisplayOff(false, "update");   // the progress bar needs to be seen
        dma_display->clearScreen();
        dma_display->setTextSize(1);
        dma_display->setTextColor(dma_display->color565(255, 200, 0));
        dma_display->setCursor(2, 10);
        dma_display->print("OTA Update");
        dma_display->setCursor(2, 19);
        dma_display->print("Starting...");
        dma_display->flipDMABuffer();
    });

    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        static int lastPct = -1;
        unsigned int pct = (total > 0) ? (progress * 100 / total) : 0;
        // Throttle: redrawing the matrix every callback would slow the
        // transfer down. Only redraw when the percentage actually changes.
        if ((int)pct == lastPct) return;
        lastPct = pct;

        Serial.printf("OTA progress: %u%%\r", pct);

        dma_display->clearScreen();
        dma_display->setTextSize(1);
        dma_display->setTextColor(dma_display->color565(255, 200, 0));
        dma_display->setCursor(2, 10);
        dma_display->print("OTA Update");
        dma_display->setCursor(2, 19);
        dma_display->print(pct);
        dma_display->print("%");

        int barWidth = map(pct, 0, 100, 0, PANEL_RES_X - 4);
        dma_display->drawRect(2, 30, PANEL_RES_X - 4, 8, dma_display->color565(80, 80, 80));
        dma_display->fillRect(2, 30, barWidth, 8, dma_display->color565(0, 200, 0));
        dma_display->flipDMABuffer();
    });

    ArduinoOTA.onEnd([]() {
        saveBrightnessNow();
        Serial.println("\nOTA update complete. Rebooting...");
        dma_display->clearScreen();
        dma_display->setTextSize(1);
        dma_display->setTextColor(dma_display->color565(0, 255, 0));
        dma_display->setCursor(2, 18);
        dma_display->print("Update done!");
        dma_display->setCursor(2, 27);
        dma_display->print("Rebooting...");
        dma_display->flipDMABuffer();
    });

    ArduinoOTA.onError([](ota_error_t error) {
        Serial.printf("OTA error [%u]\n", error);
        dma_display->clearScreen();
        dma_display->setTextSize(1);
        dma_display->setTextColor(dma_display->color565(255, 0, 0));
        dma_display->setCursor(2, 18);
        dma_display->print("OTA FAILED");
        dma_display->flipDMABuffer();
    });

    ArduinoOTA.begin();   // also starts mDNS as blockgrid-xxxx.local
    otaInitialized = true;
    MDNS.addService("http", "tcp", 80);   // advertises the web control page, so apps that browse for devices list it

    Serial.printf("OTA ready — hostname: %s.local\n", hostname.c_str());
    Serial.print("OTA password: ");
    Serial.println(otaPassword);
}

// Boot splash — "Matrix" digital rain that decodes into BLOCKGRID.
// Designed in the simulator (renderSplash() there is the reference; this
// is a straight port, same constants and algorithm).
//
// Timeline, from the first splash frame (rainStartMs):
//   0 – 2 s     film-style rain: mirrored half-width katakana and digits,
//               5x7 in a 16 x 6 grid of 6 x 8 px cells. Near columns are
//               faster and brighter, far ones slower and dimmer; each column
//               has a white head, glyphs flicker as they fall
//   2 – 6.2 s   BLOCKGRID decodes letter by letter: each position cycles
//               through random katakana for 0.75 s, then locks in green
//               with a brief white flash (letters start 0.4 s apart).
//               Rain is kept off a 1 px outline around the letters so they
//               stay readable (no dimmed box)
//   hold        the green word with rain around it (SPLASH_POST_RESOLVE_HOLD_MS)
//   fade        no new rain; what's there fades out quickly (gone in about
//               0.9 s, RAIN_FADEOUT_MS). At the same moment a glint sweeps
//               across the word and GRID turns orange as it passes
//               (SPLASH_SWEEP_MS)
//   end         BLOCK (green) GRID (orange) alone (RAIN_TEXT_ALONE_HOLD_MS)
// Everything is timed from millis(), not frame count, so it plays at the
// same speed however often drawSplashFrame() gets called (every 50 ms from
// setup()'s loops and the splash timer).
#define MR_COLS 16                      // rain grid: 16 x 6 cells of 6 x 8 px
#define MR_ROWS 6
#define MR_CW 6
#define MR_CH 8
#define SPLASH_DECODE_START_MS   2000UL // pure rain before the word starts decoding
#define SPLASH_LETTER_STAGGER_MS  400UL // each letter starts decoding this long after the previous one
#define SPLASH_DECODE_MS          750UL // how long a letter cycles through random characters
#define SPLASH_DECODE_STEP_MS      70UL // a decoding letter shows a new random character this often
#define SPLASH_FLASH_MS           350UL // white flash easing back to green as a letter locks
#define RAIN_FORCE_RESOLVED_MS   6000UL // + 200 below = 6.2 s: the last letter has locked (at 5.95 s) by then
#define SPLASH_SWEEP_MS          1200UL // glint across the word; GRID turns orange as it passes
#define RAIN_WORD_X 21                  // the word's position: centred (9 letters x 6 px)
#define RAIN_WORD_Y 21

// Rain look (same names and values as RAIN_TUNE in the simulator)
#define RAIN_NEAR_PROB     0.7f         // share of columns that are "near" (fast, bright)
#define RAIN_NEAR_V0       6.3f         // near columns: 6.3 – 10.5 rows per second
#define RAIN_NEAR_V1      10.5f
#define RAIN_FAR_V0        2.8f         // far columns: 2.8 – 5.3 rows per second
#define RAIN_FAR_V1        5.3f
#define RAIN_FAR_LEVEL     0.55f        // brightness of far columns
#define RAIN_FADE_BASE     0.43f        // trail brightness left after 1 s (higher = longer trails)
#define RAIN_FADEOUT_BASE  0.03f        // ...once the rain has stopped spawning (fast fade-out)
#define RAIN_RESPAWN_GAP   3.0f         // up to this many rows of gap before a column restarts
#define RAIN_HEAD_LEVEL    0.85f        // a cell stays a white "head" while brightness is above this
#define RAIN_TRAIL_GAMMA   0.6f         // < 1 keeps trails brighter for longer
#define RAIN_MUTATE_PER_S  1.6f         // how often a glyph flickers to another shape

const char *RAIN_TARGET = "BLOCKGRID";
const uint8_t RAIN_TARGET_LEN = 9;

// Rain glyphs: 32 half-width katakana, mirrored left-to-right like the
// film's code, then the digits 0–9. 5 columns each, bit 0 = top row
// (same format and order as KANA_GLYPHS in the simulator).
const uint8_t KANA_GLYPHS[][5] = {
    {0x07,0x09,0x1D,0x21,0x41}, {0x01,0x02,0x7C,0x08,0x10}, {0x0E,0x12,0x23,0x42,0x06}, {0x42,0x42,0x7E,0x42,0x42},
    {0x02,0x7F,0x4A,0x12,0x22}, {0x3E,0x42,0x02,0x1F,0x62}, {0x0A,0x0A,0x7E,0x0B,0x0A}, {0x0E,0x12,0x22,0x43,0x44},
    {0x02,0x1E,0x22,0x43,0x04}, {0x7F,0x41,0x41,0x41,0x41}, {0x02,0x1F,0x22,0x47,0x02}, {0x0C,0x10,0x20,0x45,0x45},
    {0x23,0x15,0x09,0x11,0x21}, {0x46,0x4A,0x42,0x3F,0x02}, {0x0E,0x12,0x2A,0x4B,0x44}, {0x0F,0x10,0x23,0x40,0x43},
    {0x04,0x05,0x1D,0x25,0x44}, {0x02,0x02,0x1F,0x22,0x42}, {0x03,0x15,0x09,0x15,0x21}, {0x22,0x16,0x7B,0x12,0x22},
    {0x38,0x06,0x00,0x0E,0x30}, {0x42,0x42,0x44,0x44,0x3F}, {0x1A,0x02,0x7F,0x02,0x1A}, {0x03,0x05,0x29,0x11,0x09},
    {0x60,0x38,0x23,0x2C,0x30}, {0x01,0x22,0x14,0x18,0x24}, {0x45,0x45,0x7F,0x05,0x05}, {0x0E,0x02,0x7A,0x07,0x02},
    {0x0C,0x15,0x25,0x45,0x44}, {0x00,0x1F,0x20,0x40,0x0F}, {0x0F,0x11,0x21,0x41,0x47}, {0x0E,0x10,0x20,0x42,0x41},
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00}, {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
};
const uint8_t KANA_GLYPH_COUNT = sizeof(KANA_GLYPHS) / sizeof(KANA_GLYPHS[0]);

static inline float randomFloat() { return random(0, 1000000) / 1000000.0f; }

struct RainCol {
    float y;          // fractional row of this column's head
    float v;          // rows per second
    bool near;        // near = fast and bright, far = slow and dim
    int16_t lastRow;  // row the head was last in (a new glyph lights on entering a row)

    // A fresh column above the top edge. (A member function rather than a
    // free one taking RainCol&, which Arduino's auto-prototypes would place
    // before this struct exists.)
    void reset(bool initial) {
        near = randomFloat() < RAIN_NEAR_PROB;
        y = -randomFloat() * (initial ? 8.0f : 4.0f);
        v = near ? RAIN_NEAR_V0 + randomFloat() * (RAIN_NEAR_V1 - RAIN_NEAR_V0)
                 : RAIN_FAR_V0 + randomFloat() * (RAIN_FAR_V1 - RAIN_FAR_V0);
        lastRow = -99;
    }
};
RainCol rainCols[MR_COLS];
uint8_t rainGlyph[MR_ROWS][MR_COLS];    // 0xFF = empty
float   rainBright[MR_ROWS][MR_COLS];   // 0..1, fades over time
bool    rainNear[MR_ROWS][MR_COLS];
uint8_t splashHalo[48][12];             // 1 bit per pixel: no rain here (outline around the letters)
unsigned long rainStartMs = 0;
unsigned long rainLastStepMs = 0;
unsigned long splashFadeStartMs = 0;    // when spawning stopped (the sweep starts then); 0 = not yet
bool rainInitialized = false;


void initRain() {
    for (uint8_t c = 0; c < MR_COLS; c++) rainCols[c].reset(true);
    for (uint8_t r = 0; r < MR_ROWS; r++)
        for (uint8_t c = 0; c < MR_COLS; c++) { rainGlyph[r][c] = 0xFF; rainBright[r][c] = 0.0f; rainNear[r][c] = false; }
    rainStartMs = rainLastStepMs = millis();
    splashFadeStartMs = 0;
}

void stepRain(bool allowSpawning) {
    unsigned long now = millis();
    float dt = (now - rainLastStepMs) / 1000.0f;
    if (dt > 0.25f) dt = 0.25f;          // after a long gap, resume rather than jump
    rainLastStepMs = now;
    // Trails fade out over about a second; once the rain stops spawning they
    // fade much faster, so the rain is gone in ~0.9 s and the word stands alone.
    float fade = powf(allowSpawning ? RAIN_FADE_BASE : RAIN_FADEOUT_BASE, dt);
    for (uint8_t r = 0; r < MR_ROWS; r++)
        for (uint8_t c = 0; c < MR_COLS; c++) rainBright[r][c] *= fade;
    for (uint8_t c = 0; c < MR_COLS; c++) {
        RainCol &col = rainCols[c];
        col.y += col.v * dt;
        int16_t r = (int16_t)floorf(col.y);
        if (r != col.lastRow) {
            col.lastRow = r;
            if (r >= 0 && r < MR_ROWS && allowSpawning) {
                rainGlyph[r][c] = random(0, KANA_GLYPH_COUNT);
                rainBright[r][c] = 1.0f;
                rainNear[r][c] = col.near;
            }
        }
        if (col.y > MR_ROWS + randomFloat() * RAIN_RESPAWN_GAP) col.reset(false);
    }
    // Glyphs flicker to a different shape while they fade.
    float pMutate = RAIN_MUTATE_PER_S * dt;
    for (uint8_t r = 0; r < MR_ROWS; r++)
        for (uint8_t c = 0; c < MR_COLS; c++)
            if (rainGlyph[r][c] != 0xFF && randomFloat() < pMutate) rainGlyph[r][c] = random(0, KANA_GLYPH_COUNT);
}

static inline float splashEase(float t) { return t * t * (3.0f - 2.0f * t); }

// Letter i of the word at time t, as 5 columns (bit 0 = top row).
// Returns 0 = not started yet, 1 = decoding (random katakana), 2 = settled.
uint8_t splashLetterCols(uint8_t i, unsigned long t, uint8_t cols[5]) {
    unsigned long t0 = SPLASH_DECODE_START_MS + i * SPLASH_LETTER_STAGGER_MS, t1 = t0 + SPLASH_DECODE_MS;
    if (t < t0) return 0;
    if (t < t1) {
        uint32_t h = ((t / SPLASH_DECODE_STEP_MS) * 31u + i) * 2654435761u;   // new random character every SPLASH_DECODE_STEP_MS
        memcpy(cols, KANA_GLYPHS[h % KANA_GLYPH_COUNT], 5);
        return 1;
    }
    // Settled: the letter from the panel font, unpacked into columns.
    memset(cols, 0, 5);
    const GFXglyph &gl = BlockGridFont5x7Glyphs[(uint8_t)RAIN_TARGET[i] - 0x20];
    const uint8_t *bm = BlockGridFont5x7Bitmaps + gl.bitmapOffset;
    uint8_t bits = 0, bit = 0;
    for (uint8_t yy = 0; yy < gl.height; yy++)
        for (uint8_t xx = 0; xx < gl.width; xx++) {
            if (!(bit++ & 7)) bits = *bm++;
            if ((bits & 0x80) && xx < 5 && yy < 8) cols[xx] |= 1 << yy;
            bits <<= 1;
        }
    return 2;
}

void drawSplashFrame(bool allowSpawning = true) {
    if (!rainInitialized) { initRain(); rainInitialized = true; }
    if (!allowSpawning && splashFadeStartMs == 0) splashFadeStartMs = millis();
    stepRain(allowSpawning);
    unsigned long t = millis() - rainStartMs;

    // What each letter shows this frame, and the 1 px outline around the
    // letters that the rain keeps off.
    uint8_t letterCols[RAIN_TARGET_LEN][5];
    uint8_t letterState[RAIN_TARGET_LEN];
    memset(splashHalo, 0, sizeof(splashHalo));
    for (uint8_t i = 0; i < RAIN_TARGET_LEN; i++) {
        letterState[i] = splashLetterCols(i, t, letterCols[i]);
        if (!letterState[i]) continue;
        for (int8_t cx = 0; cx < 5; cx++)
            for (int8_t ry = 0; ry < 8; ry++) {
                if (!((letterCols[i][cx] >> ry) & 1)) continue;
                for (int8_t dy = -1; dy <= 1; dy++)
                    for (int8_t dx = -1; dx <= 1; dx++) {
                        int16_t px = RAIN_WORD_X + i * 6 + cx + dx, py = RAIN_WORD_Y + ry + dy;
                        if (px >= 0 && px < 96 && py >= 0 && py < 48) splashHalo[py][px >> 3] |= 0x80 >> (px & 7);
                    }
            }
    }

    dma_display->clearScreen();

    // Rain
    for (uint8_t r = 0; r < MR_ROWS; r++) {
        for (uint8_t c = 0; c < MR_COLS; c++) {
            float b = rainBright[r][c];
            if (b < 0.04f || rainGlyph[r][c] == 0xFF) continue;
            int16_t x = c * MR_CW, y = r * MR_CH;
            float k = rainNear[r][c] ? 1.0f : RAIN_FAR_LEVEL;
            float bt = powf(b, RAIN_TRAIL_GAMMA);
            uint16_t color = b > RAIN_HEAD_LEVEL
                ? dma_display->color565((uint8_t)(215 * k), (uint8_t)(255 * k), (uint8_t)(215 * k))   // bright head
                : dma_display->color565(0, (uint8_t)(255 * bt * k), (uint8_t)(70 * bt * k));        // fading green trail
            const uint8_t *g = KANA_GLYPHS[rainGlyph[r][c]];
            for (uint8_t cx = 0; cx < 5; cx++)
                for (uint8_t ry = 0; ry < 7; ry++) {
                    int16_t px = x + cx, py = y + ry;
                    if (!((g[cx] >> ry) & 1) || px >= 96 || py >= 48) continue;
                    if (splashHalo[py][px >> 3] & (0x80 >> (px & 7))) continue;
                    dma_display->drawPixel(px, py, color);
                }
        }
    }

    // The sweep (glint + GRID turning orange) starts when the rain starts fading.
    float sweepP = -1.0f, bandX = -99.0f;
    if (splashFadeStartMs != 0) {
        sweepP = (millis() - splashFadeStartMs) / (float)SPLASH_SWEEP_MS;
        if (sweepP > 1.0f) sweepP = 1.0f;
        bandX = RAIN_WORD_X - 6 + (RAIN_TARGET_LEN * 6 + 12) * splashEase(sweepP);
    }
    fxSweep.on = sweepP >= 0.0f && sweepP < 1.0f;
    fxSweep.x0 = RAIN_WORD_X; fxSweep.x1 = RAIN_WORD_X + RAIN_TARGET_LEN * 6; fxSweep.bx = bandX;

    // The word: each letter decodes (random katakana), then locks with a flash.
    for (uint8_t i = 0; i < RAIN_TARGET_LEN; i++) {
        if (!letterState[i]) continue;
        int16_t x = RAIN_WORD_X + i * 6;
        uint8_t cr = 120, cg = 255, cb = 140;              // decoding
        if (letterState[i] == 2) {
            // Settled colour: green, except GRID turns orange as the sweep passes it.
            unsigned long t1 = SPLASH_DECODE_START_MS + i * SPLASH_LETTER_STAGGER_MS + SPLASH_DECODE_MS;
            float lr = 200, lg = 255, lb = 200;
            if (i >= 5) {
                float cx = x + 2.5f;
                float k = (bandX - (cx - 3.0f)) / 6.0f;
                if (k < 0) k = 0; if (k > 1) k = 1;
                lr = 200 + (255 - 200) * k; lg = 255 + (159 - 255) * k; lb = 200 + (28 - 200) * k;
            }
            float f = (t - t1) / (float)SPLASH_FLASH_MS;
            if (f > 1) f = 1;
            cr = (uint8_t)lroundf(255 + (lr - 255) * f); cg = (uint8_t)lroundf(255 + (lg - 255) * f); cb = (uint8_t)lroundf(255 + (lb - 255) * f);
        }
        for (uint8_t cx = 0; cx < 5; cx++)
            for (uint8_t ry = 0; ry < 8; ry++)
                if ((letterCols[i][cx] >> ry) & 1) fxPixel(x + cx, RAIN_WORD_Y + ry, cr, cg, cb);
    }
    fxSweep.on = false;
    dma_display->flipDMABuffer();
}

// ---------- Setup ----------
void setup() {
    Serial.begin(115200);
    Serial.printf("\nBlockGrid v%s\n", BLOCKGRID_VERSION);
    randomSeed(esp_random());  // hardware RNG — without this, the rain splash below would look identical every single boot

    preferences.begin("wifiCreds", false);
    String ssid     = preferences.getString("ssid", "");
    String password = preferences.getString("password", "");
    savedSsid = ssid;
    savedPassword = password;

    // Restore the last-selected brand/currency. Clamped in case a firmware
    // update ever ships with fewer options than what was previously saved.
    currentBrandIndex = preferences.getUChar("brand", 0);
    if (currentBrandIndex >= NUM_BRAND_PROFILES) currentBrandIndex = 0;
    currentPriceFx = preferences.getUChar("priceFx", FX_FLASH);
    if (currentPriceFx >= NUM_PRICE_FX) currentPriceFx = FX_FLASH;
    currentPriceSrc = preferences.getUChar("priceSrc", PRICE_SRC_AUTO);
    if (currentPriceSrc >= NUM_PRICE_SRC) currentPriceSrc = PRICE_SRC_AUTO;
    currentBlockSrc = preferences.getUChar("blockSrc", BLOCK_SRC_AUTO);
    nodeUrl = preferences.getString("nodeUrl", "");
    if (!normaliseNodeUrl(nodeUrl)) nodeUrl = "";
    if (currentBlockSrc >= NUM_BLOCK_SRC) currentBlockSrc = BLOCK_SRC_AUTO;
    currentInterval = preferences.getUChar("interval", DEFAULT_INTERVAL_INDEX);
    if (currentInterval >= NUM_INTERVALS) currentInterval = DEFAULT_INTERVAL_INDEX;
    // Saved as the Currency value (USD 0, JPY 1, EUR 2), not the list position.
    currentCurrencyIndex = currencyIndexOf(preferences.getUChar("currency", (uint8_t)Currency::USD));
    currentBrightness = preferences.getUChar("brightness", 60);
    if (currentBrightness < BRIGHTNESS_MIN) currentBrightness = BRIGHTNESS_MIN;
    if (currentBrightness > BRIGHTNESS_MAX) currentBrightness = BRIGHTNESS_MAX;

#if HAS_BUTTONS
    pinMode(BUTTON_BRAND_PIN, INPUT_PULLUP);
    pinMode(BUTTON_CURRENCY_PIN, INPUT_PULLUP);
#endif

    // TLS: no certificate pinning — setInsecure() on each of the two
    // persistent, per-host clients declared above (coingeckoClient,
    // mempoolClient), called once here rather than per-request. See the
    // comment by their declaration for why one-persistent-client-per-host
    // replaced both the original single-shared-client design (broke across
    // different hosts) and the fresh-client-per-request design that
    // replaced it in turn (suspected of fragmenting heap over a long run).
    coingeckoClient.setInsecure();
    mempoolClient.setInsecure();
    // The TLS library waits up to 120 s for a secure handshake that has
    // stalled (a lost packet, a busy server) — far longer than any of the
    // HTTP timeouts, and long enough to hold up the whole boot. A healthy
    // handshake takes under a second here (the boot fetch does three in
    // about 2 s), so give up after 5 and let the backup server take over —
    // the display, buttons and web page wait for as long as this takes.
    coingeckoClient.setHandshakeTimeout(5);   // seconds
    mempoolClient.setHandshakeTimeout(5);

    // Matrix panel config
    HUB75_I2S_CFG mxconfig(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN);
    mxconfig.gpio.e  = E_PIN_DEFAULT;
    mxconfig.clkphase = false;
    mxconfig.driver  = HUB75_I2S_CFG::FM6126A;
#if PANEL_VERSION == 2
    // SM5368 shift-register row drivers: A is the row clock, B blanking and
    // C the row data, instead of A-E being a binary row address (D and E go
    // unused). Without this, each row drawn lights 12 of the 24 rows in its
    // half, producing a jumbled image.
    mxconfig.line_decoder = HUB75_I2S_CFG::SM5368;
    // Clock deliberately left at the library's default 8 MHz, even though
    // Waveshare's V2 instructions use 16 MHz. On this build 16 MHz caused
    // two problems: about 18 KB more DMA memory (which starved the TLS
    // connections), and, with the panel and ESP32 sharing the barrel-jack
    // supply, the faster switching stopped it booting at all (artefacts,
    // no splash). 8 MHz displays correctly, with the same memory use and
    // refresh rate (~89 Hz) as the V1 panel.
    // Red and blue are swapped on V2: tell the library which wire is which
    // (no rewiring needed).
    mxconfig.gpio.r1 = B1_PIN_DEFAULT; mxconfig.gpio.b1 = R1_PIN_DEFAULT;
    mxconfig.gpio.r2 = B2_PIN_DEFAULT; mxconfig.gpio.b2 = R2_PIN_DEFAULT;
#endif
    // Without this, every clearScreen() + redraw sequence draws directly
    // into the exact same buffer the DMA engine is actively scanning out
    // to the physical LEDs — meaning the brief black flash from
    // clearScreen(), followed by each element "popping in" as its own
    // print() call lands, is genuinely visible in real time on every
    // refresh. With double buffering, drawing happens on an off-screen
    // back buffer, invisible until dma_display->flipDMABuffer() swaps it
    // in — the viewer only ever sees a complete old frame or a complete
    // new frame, never a partial one. Every function that draws a full
    // screen ends with a flipDMABuffer() call — see each one for why.
    mxconfig.double_buff = true;

    dma_display = new MatrixPanel_I2S_DMA(mxconfig);
    dma_display->begin();
    dma_display->setBrightness8(currentBrightness);
    dma_display->clearScreen();
    dma_display->flipDMABuffer(); // makes the initial blank screen actually visible — with double buffering, the front buffer otherwise stays whatever it was until the first flip
    // the extended (128-255) character range for backward compatibility —
    // characters like ¥ (0x9D) render as the wrong glyph unless this is
    // explicitly enabled. See adafruit/Adafruit-GFX-Library#22.
    dma_display->cp437(true);
    // All panel text uses the simulator-matching font (see BlockGridFont5x7).
    dma_display->setFont(&BlockGridFont5x7);
    titleCanvas.setFont(&BlockGridFont5x7);

    // Boot splash — shown from here until this function's end, covering
    // WiFi connecting, the initial data fetch, and (see below) a minimum
    // on-screen duration so the rain effect always gets to fully play out.
    drawSplashFrame();

    if (ssid.isEmpty()) {
        Serial.println("No WiFi credentials stored. Starting AP mode.");
        startAccessPoint();
        return;
    }

    // Retry a few times with fresh connection attempts rather than one long
    // wait — some boards see a slower or occasionally-stuck first reconnect
    // right after an OTA-triggered reboot compared to a normal cold boot,
    // and a stale stuck attempt often clears up with a fresh WiFi.begin()
    // rather than just waiting longer on the same one. If this still lands
    // in AP mode, check Serial output: "No WiFi credentials stored" means
    // the stored SSID/password is genuinely empty (a real, separate
    // problem, since Preferences/NVS lives in its own flash partition and
    // OTA shouldn't touch it) — "Connection failed after all retries"
    // means the credentials are fine and this was purely a reconnect
    // timing issue.
    const uint8_t WIFI_CONNECT_ATTEMPTS = 3;
    bool wifiConnected = false;
    for (uint8_t attempt = 1; attempt <= WIFI_CONNECT_ATTEMPTS && !wifiConnected; attempt++) {
        if (attempt > 1) {
            Serial.printf("\nRetrying WiFi connection (attempt %u/%u)...\n", attempt, WIFI_CONNECT_ATTEMPTS);
            WiFi.disconnect();
            delay(200);
            drawSplashFrame();
        }
        WiFi.setHostname(getDeviceHostname().c_str());   // how this device appears in your router's list (must be set before begin())
        WiFi.begin(ssid.c_str(), password.c_str());
        // No Wi-Fi power saving: with it on, the radio naps between beacons,
        // which slows the control page's replies and makes blockgrid-xxxx.local
        // lookups fail now and then. The extra power is tiny next to the LEDs.
        WiFi.setSleep(false);
        Serial.print("Connecting to WiFi");

        unsigned long startTime = millis();
        uint8_t pollCount = 0;
        while (WiFi.status() != WL_CONNECTED) {
            delay(50); // short poll interval so the rain splash gets smooth redraws while we wait
            if (++pollCount % 10 == 0) Serial.print("."); // roughly the original once-per-500ms cadence, so the log doesn't flood at the new 5x poll rate
            drawSplashFrame();  // keeps the rain animating while we wait
            if (millis() - startTime > WIFI_TIMEOUT_MS) {
                break;  // this attempt timed out — loop will retry or give up
            }
        }
        wifiConnected = (WiFi.status() == WL_CONNECTED);
    }

    if (!wifiConnected) {
        Serial.println("\nConnection failed after all retries. Starting AP mode.");
        startAccessPoint();
        return;
    }
    Serial.println("\nConnected to WiFi: " + WiFi.localIP().toString());
    currentState = DeviceState::RUNNING;
    setupOTA();
    startWebControl();   // reachable from here on; reports "starting up" until the splash ends

    // Boot splash sequence. Every phase boundary below logs how long that
    // phase took, so if the on-screen timing ever looks wrong, the Serial
    // log shows which phase is responsible.
    //
    // Phases:
    //   1. The first price/block fetch, while a timer keeps the rain
    //      animating (see splashTimerCallback()), so the splash doesn't
    //      freeze for a second or more while the HTTPS requests are in flight.
    //   2. Animate until the word is guaranteed fully resolved
    //      (RAIN_FORCE_RESOLVED_MS, measured from rainStartMs).
    //   3. Rain keeps flowing for SPLASH_POST_RESOLVE_HOLD_MS after the
    //      word resolves.
    //   4. Existing rain fades out (RAIN_FADEOUT_MS) with no new glyphs
    //      spawning, while a glint sweeps the word and GRID turns orange
    //      (see drawSplashFrame()).
    //   5. Hold on the bare word alone for RAIN_TEXT_ALONE_HOLD_MS.
    unsigned long phaseStart = millis();
    startSplashRenderer();
    fetchAndCacheBitcoinData(false);
    stopSplashRenderer();
    Serial.printf("Splash phase 1 (fetch, animated): %lums\n", millis() - phaseStart);

    phaseStart = millis();
    while (millis() - rainStartMs < RAIN_FORCE_RESOLVED_MS + 200) {
        drawSplashFrame();
        delay(50);
    }
    Serial.printf("Splash phase 2 (resolve): %lums\n", millis() - phaseStart);

    phaseStart = millis();
    while (millis() - phaseStart < SPLASH_POST_RESOLVE_HOLD_MS) {
        drawSplashFrame();
        delay(50);
    }
    Serial.printf("Splash phase 3 (post-resolve hold): %lums\n", millis() - phaseStart);

    phaseStart = millis();
    while (millis() - phaseStart < RAIN_FADEOUT_MS) {
        drawSplashFrame(false); // no new glyphs — existing rain fades out while the GRID sweep plays
        delay(50);
    }
    Serial.printf("Splash phase 4 (fade-out): %lums\n", millis() - phaseStart);

    phaseStart = millis();
    while (millis() - phaseStart < RAIN_TEXT_ALONE_HOLD_MS) {
        drawSplashFrame(false); // the sweep finishes, then green BLOCK, orange GRID on their own
        delay(50);
    }
    Serial.printf("Splash phase 5 (text alone): %lums\n", millis() - phaseStart);

    // This also sets lastUpdateMs, so loop() won't immediately redo this
    // same fetch on its first pass.
    displayBitcoinInfo(currentSnapshot());
    lastUpdateMs = millis();
    bootComplete = true;   // the web control page's settings are live from here
}

// ---------- Data fetching ----------
// Identifies BlockGrid to the price and block servers. HTTPClient's own
// default ("ESP32HTTPClient") is what a lot of scripted traffic sends, and
// CoinGecko's firewall (Cloudflare) started answering it with 403 Forbidden.
#define BLOCKGRID_USER_AGENT "BlockGrid/1.0 (ESP32 Bitcoin ticker)"

// After CoinGecko refuses a request (403 Forbidden or 429 Too Many
// Requests), it isn't asked again for a while; mempool.space supplies the
// price meanwhile (in Auto). The pause grows with each refusal in a row —
// 10 min, then 30 min, then 1 hour for as long as it keeps refusing — and
// goes back to the start after CoinGecko answers again. Asking often while
// blocked only prolongs a block.
const uint8_t COINGECKO_BACKOFF_MIN[] = { 10, 30, 60 };
const uint8_t NUM_COINGECKO_BACKOFFS = sizeof(COINGECKO_BACKOFF_MIN) / sizeof(COINGECKO_BACKOFF_MIN[0]);
unsigned long coingeckoBackoffUntilMs = 0;   // 0 = not backing off
uint8_t coingeckoRefusals = 0;               // refusals in a row (reset by a success)

// The TLS library's reason for a failed secure connection, in words. Its
// code -1 covers "couldn't connect" and "the secure handshake ran out of
// time" (setHandshakeTimeout()), which it otherwise prints as the
// unhelpful "ERROR - Generic error".
void tlsReason(WiFiClientSecure &client, char *buf, size_t n) {
    buf[0] = 0;
    if (client.lastError(buf, n) == -1)
        snprintf(buf, n, "no connection, or the secure handshake timed out");
}

// Prints why an HTTPS request failed: the TLS library's reason for
// connection errors (negative codes), or the start of the server's reply for
// HTTP errors like 403 (shows whether it's a firewall block or a rate limit).
void logFetchFailure(const char *what, const char *host, int httpCode, HTTPClient &http, WiFiClientSecure &client) {
    if (httpCode < 0) {
        char tlsErr[100] = "";
        tlsReason(client, tlsErr, sizeof(tlsErr));
        Serial.printf("HTTP error fetching %s from %s: %d (%s)%s%s\n", what, host, httpCode,
                      http.errorToString(httpCode).c_str(), tlsErr[0] ? " — TLS: " : "", tlsErr);
    } else {
        char snippet[121];
        size_t n = 0;
        WiFiClient *s = http.getStreamPtr();
        unsigned long t0 = millis();
        while (s && n < sizeof(snippet) - 1 && millis() - t0 < 1000) {
            int c = s->read();
            if (c < 0) { if (!s->connected()) break; delay(5); continue; }
            snippet[n++] = (c == '\n' || c == '\r') ? ' ' : (char)c;
        }
        snippet[n] = 0;
        Serial.printf("HTTP error fetching %s from %s: %d — reply: \"%s\"\n", what, host, httpCode, snippet);
    }
    Serial.printf("  WiFi.status()=%d (3=connected), IP=%s, RSSI=%d dBm, freeHeap=%u, largestBlock=%u\n",
                   WiFi.status(), WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                   ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

// USD and JPY prices plus their 24h change, from CoinGecko. See
// fetchBitcoinPrices() below for the fallback when this fails.
bool fetchPricesFromCoinGecko(float &usdOut, float &jpyOut, float &usdChangeOut, float &jpyChangeOut, float &eurOut, float &eurChangeOut) {
    HTTPClient http;
    // HTTP/1.0 so the server can't reply with chunked transfer encoding —
    // required for parsing straight from http.getStream() below, which
    // (unlike getString()) doesn't strip chunk headers. This is the setup
    // ArduinoJson's own HTTPClient example uses. Costs nothing here: the
    // connection is torn down by http.end() every cycle anyway.
    http.useHTTP10(true);
    http.begin(coingeckoClient, "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd,eur,jpy&include_24hr_change=true");
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setUserAgent(BLOCKGRID_USER_AGENT);
    http.addHeader("Accept", "application/json");
    // NOTE: this is a public, unauthenticated endpoint — no API key needed.

    bool ok = false;
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        // Parsed directly from the network stream rather than first copying
        // the whole response into a String — one less heap allocation per
        // fetch, which matters given the fragmentation seen in the logs.
        JsonDocument doc;  // ArduinoJson v7+; use DynamicJsonDocument(1024) on v6
        if (deserializeJson(doc, http.getStream()) == DeserializationError::Ok) {
            usdOut = doc["bitcoin"]["usd"].as<float>();
            jpyOut = doc["bitcoin"]["jpy"].as<float>();
            eurOut = doc["bitcoin"]["eur"].as<float>();
            // The 24h change can come back null or be absent. That's kept as
            // NAN ("unknown") rather than as<float>()'s default of 0.0, so
            // the display hides the change indicator instead of showing a
            // made-up "▲0.0%".
            JsonVariantConst usdChg = doc["bitcoin"]["usd_24h_change"];
            JsonVariantConst jpyChg = doc["bitcoin"]["jpy_24h_change"];
            usdChangeOut = usdChg.isNull() ? NAN : usdChg.as<float>();
            jpyChangeOut = jpyChg.isNull() ? NAN : jpyChg.as<float>();
            JsonVariantConst eurChg = doc["bitcoin"]["eur_24h_change"];
            eurChangeOut = eurChg.isNull() ? NAN : eurChg.as<float>();
            Serial.printf("BTC price: $%.0f (%+.2f%%) / €%.0f (%+.2f%%) / ¥%.0f (%+.2f%%)\n",
                          usdOut, usdChangeOut, eurOut, eurChangeOut, jpyOut, jpyChangeOut);
            ok = (usdOut > 0.0f && jpyOut > 0.0f && eurOut > 0.0f);
        } else {
            Serial.println("JSON parse error (price)");
        }
    } else {
        logFetchFailure("BTC price", "api.coingecko.com", httpCode, http, coingeckoClient);
        if (httpCode == 403 || httpCode == 429) {
            uint8_t step = coingeckoRefusals < NUM_COINGECKO_BACKOFFS ? coingeckoRefusals : NUM_COINGECKO_BACKOFFS - 1;
            if (coingeckoRefusals < 255) coingeckoRefusals++;
            coingeckoBackoffUntilMs = millis() + COINGECKO_BACKOFF_MIN[step] * 60000UL;
            if (coingeckoBackoffUntilMs == 0) coingeckoBackoffUntilMs = 1;
            Serial.printf("CoinGecko refused the request (%u in a row) — not asking it again for %u min%s\n",
                          coingeckoRefusals, COINGECKO_BACKOFF_MIN[step],
                          currentPriceSrc == PRICE_SRC_AUTO ? "; mempool.space supplies the price meanwhile" : "");
        }
    }
    http.end();
    coingeckoClient.stop();   // free the TLS buffers before the next request needs its own
    return ok;
}

// Backup price source: mempool.space's own price feed, one small reply with
// USD, EUR and JPY, e.g. {"time":1790626204,"USD":83541,"EUR":77450,...,"JPY":13142452}.
// It has no 24h change; that comes from Coinbase (see below).
bool fetchPricesFromMempool(float &usdOut, float &jpyOut, float &eurOut) {
    HTTPClient http;
    http.useHTTP10(true);
    if (!http.begin(mempoolClient, "https://mempool.space/api/v1/prices")) return false;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setReuse(false);
    http.setUserAgent(BLOCKGRID_USER_AGENT);

    bool ok = false;
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        JsonDocument doc;
        if (deserializeJson(doc, http.getStream()) == DeserializationError::Ok) {
            usdOut = doc["USD"].as<float>();
            jpyOut = doc["JPY"].as<float>();
            eurOut = doc["EUR"].as<float>();
            Serial.printf("BTC price: $%.0f / €%.0f / ¥%.0f (mempool.space)\n", usdOut, eurOut, jpyOut);
            ok = (usdOut > 0.0f && jpyOut > 0.0f && eurOut > 0.0f);
        } else {
            Serial.println("JSON parse error (mempool.space price)");
        }
    } else {
        logFetchFailure("BTC price", "mempool.space", httpCode, http, mempoolClient);
    }
    http.end();
    mempoolClient.stop();
    return ok;
}

// Backup 24h change: Coinbase Exchange's 24-hour stats for BTC-USD, e.g.
// {"open":"84116.73","high":...,"low":...,"last":"83491.9",...}, where
// "open" is the price exactly 24 hours ago. Coinbase has no yen market, so
// this gives the dollar change only. Numbers arrive as strings.
bool fetchUsdChangeFromCoinbase(float &changeOut) {
    HTTPClient http;
    http.useHTTP10(true);
    // coingeckoClient is free here: CoinGecko was just tried (and stopped)
    // or is backing off, and stop() leaves no connection state behind.
    if (!http.begin(coingeckoClient, "https://api.exchange.coinbase.com/products/BTC-USD/stats")) return false;
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setReuse(false);
    http.setUserAgent(BLOCKGRID_USER_AGENT);   // Coinbase refuses requests without one

    bool ok = false;
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        JsonDocument doc;
        if (deserializeJson(doc, http.getStream()) == DeserializationError::Ok) {
            float open = atof(doc["open"] | "0");
            float last = atof(doc["last"] | "0");
            if (open > 0.0f && last > 0.0f) {
                changeOut = (last / open - 1.0f) * 100.0f;
                ok = true;
            }
        } else {
            Serial.println("JSON parse error (Coinbase stats)");
        }
    } else {
        logFetchFailure("24h change", "api.exchange.coinbase.com", httpCode, http, coingeckoClient);
    }
    http.end();
    coingeckoClient.stop();
    return ok;
}

// The last real 24h change and when it arrived. If the change can't be
// fetched this time (the price itself came through), the last one is shown
// for up to CHANGE_KEEP_MS instead of hiding the ▲/▼ indicator — it's a
// 24-hour figure, so a few minutes old is still right.
#define CHANGE_KEEP_MS (30UL * 60UL * 1000UL)
float keptChangeUsd = NAN, keptChangeJpy = NAN, keptChangeEur = NAN;
unsigned long keptChangeMs = 0;
void rememberChange(float usd, float jpy, float eur) {
    if (isnan(usd)) return;
    keptChangeUsd = usd; keptChangeJpy = jpy; keptChangeEur = eur; keptChangeMs = millis();
}

// Fetches USD and JPY prices. CoinGecko first — one request gives both
// prices and both 24h changes, aggregated from real exchange data
// (including JPY-native venues; the Coinbase feed this replaced once showed
// a badly wrong JPY price). If CoinGecko fails or is backing off after
// refusing us, mempool.space supplies the prices and the 24h change is
// shown as unknown.
bool fetchBitcoinPrices(float &usdOut, float &jpyOut, float &usdChangeOut, float &jpyChangeOut, float &eurOut, float &eurChangeOut) {
    // Logged every cycle (success or failure) to keep watching the heap
    // trend — largest free block matters more than total free heap here,
    // since fragmentation can starve a new TLS buffer allocation even when
    // total free bytes look fine.
    Serial.printf("Free heap: %u bytes, largest block: %u bytes\n",
                   ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    // Which sources to use comes from the web page's "Data sources" setting.
    bool backingOff = coingeckoBackoffUntilMs != 0 && (long)(millis() - coingeckoBackoffUntilMs) < 0;
    if (currentPriceSrc != PRICE_SRC_BACKUP && !backingOff) {
        coingeckoBackoffUntilMs = 0;
        if (fetchPricesFromCoinGecko(usdOut, jpyOut, usdChangeOut, jpyChangeOut, eurOut, eurChangeOut)) {
            if (coingeckoRefusals) Serial.println("CoinGecko is answering again");
            coingeckoRefusals = 0;
            lastPriceFrom = "CoinGecko";
            rememberChange(usdChangeOut, jpyChangeOut, eurChangeOut);
            return true;
        }
    }
    if (currentPriceSrc == PRICE_SRC_COINGECKO) return false;   // CoinGecko only: no backup
    // Backup: prices from mempool.space, 24h change from Coinbase. The euro
    // and yen changes are estimated as the dollar change (the real one also includes
    // that day's dollar-yen exchange rate move, usually a few tenths of a
    // percent), so the arrow direction is right and the number close.
    usdChangeOut = NAN;
    jpyChangeOut = NAN;
    eurChangeOut = NAN;
    if (!fetchPricesFromMempool(usdOut, jpyOut, eurOut)) return false;
    lastPriceFrom = "mempool.space";
    float change;
    if (fetchUsdChangeFromCoinbase(change)) {
        usdChangeOut = change;
        jpyChangeOut = change;
        eurChangeOut = change;
        lastPriceFrom = "mempool.space + Coinbase";
        Serial.printf("24h change: %+.2f%% (Coinbase; euro and yen %% estimated from it)\n", change);
        rememberChange(change, change, change);
    } else if (!isnan(keptChangeUsd) && millis() - keptChangeMs < CHANGE_KEEP_MS) {
        usdChangeOut = keptChangeUsd;
        jpyChangeOut = keptChangeJpy;
        eurChangeOut = keptChangeEur;
        lastPriceFrom = "mempool.space + Coinbase";
        Serial.printf("24h change unavailable — keeping the last one (%+.2f%%, %lu min old)\n",
                      keptChangeUsd, (millis() - keptChangeMs) / 60000UL);
    }
    return true;
}

// Sats per dollar: 100,000,000 / BTC price  (result fits in a float fine)
float calculateSatoshisPerDollar(float bitcoinPrice) {
    if (bitcoinPrice <= 0.0f) return 0.0f;
    return 100000000.0f / bitcoinPrice;
}

// Sats per ¥100 — a single yen is worth little enough that sats-per-¥1 would
// be an unsatisfyingly small number; ¥100 (a common physical coin) keeps the
// displayed figure in a similar range to the USD version.
float calculateSatoshisPer100Yen(float bitcoinPriceJPY) {
    if (bitcoinPriceJPY <= 0.0f) return 0.0f;
    return (100.0f * 100000000.0f) / bitcoinPriceJPY;
}

// Block height is an integer; return uint32_t to avoid float/int confusion.
// Asks mempool.space first; if that fails, blockstream.info, which runs the
// same open-source block explorer API (identical /blocks/tip/height reply).
// mempool.space refuses roughly one connection in six even
// with plenty of free memory, so a second source keeps the block row
// current instead of waiting a minute for the next try.
// Which server(s) to ask comes from the web page's "Data sources" setting.
// In Auto, whichever server answered last is asked first, so while one is
// having a bad spell BlockGrid doesn't wait for it to time out every minute.
bool blockstreamFirst = false;
uint32_t blockFromMempool()     { return fetchBlockHeightFrom("mempool.space", "https://mempool.space/api/blocks/tip/height", false); }
uint32_t blockFromBlockstream() { return fetchBlockHeightFrom("blockstream.info", "https://blockstream.info/api/blocks/tip/height", false); }
// Your own node's mempool — the same API as mempool.space's.
uint32_t blockFromNode() {
    String url = nodeUrl + "/api/blocks/tip/height";
    uint32_t h = fetchBlockHeightFrom("My node", url.c_str(), nodeUrl.startsWith("http://"));
    nodeOk = h > 0 ? 1 : 0;
    strlcpy(nodeErr, h > 0 ? "" : blockErr, sizeof(nodeErr));
    return h;
}
uint32_t fetchBlockHeight() {
    if (currentBlockSrc == BLOCK_SRC_MEMPOOL) return blockFromMempool();
    if (currentBlockSrc == BLOCK_SRC_BLOCKSTREAM) return blockFromBlockstream();
    if (currentBlockSrc == BLOCK_SRC_NODE && nodeUrl.length()) {
        uint32_t h = blockFromNode();
        if (h > 0) return h;
        Serial.println("My node didn't answer — asking the public servers");
    }
    uint32_t h = blockstreamFirst ? blockFromBlockstream() : blockFromMempool();
    if (h == 0) {
        h = blockstreamFirst ? blockFromMempool() : blockFromBlockstream();
        if (h > 0) {
            blockstreamFirst = !blockstreamFirst;
            Serial.printf("Block height: asking %s first from now on\n", blockstreamFirst ? "blockstream.info" : "mempool.space");
        }
    }
    return h;
}

// One block-height request to one server. The HTTPS servers (your node's
// too) share mempoolClient, stopped after every request, so no connection
// state carries over from one host to the next. plainHttp: your node at an
// http:// address, through nodePlainClient.
uint32_t fetchBlockHeightFrom(const char *host, const char *url, bool plainHttp) {
    HTTPClient http;
    uint32_t blockHeight = 0;
    WiFiClient &client = plainHttp ? nodePlainClient : (WiFiClient &)mempoolClient;
    strlcpy(blockErr, "request failed", sizeof(blockErr));
    if (!http.begin(client, url)) {
        Serial.printf("Failed to start request to %s\n", host);
        return 0;
    }
    http.setTimeout(HTTP_TIMEOUT_MS);
    // Close the connection after every request instead of keeping it alive
    // for the next one (a minute away — the server drops it long before
    // then anyway). A kept-alive HTTPS connection holds on to its ~33 KB of
    // TLS buffers the whole time.
    http.setReuse(false);
    http.setUserAgent(BLOCKGRID_USER_AGENT);

    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        String body = http.getString();
        uint32_t parsed = (uint32_t)body.toInt();

        if (parsed > MIN_PLAUSIBLE_BLOCK_HEIGHT) {
            blockHeight = parsed;
            lastBlockFrom = host;
            blockErr[0] = '\0';
            Serial.printf("Block height: %u (%s)\n", blockHeight, host);
        } else {
            Serial.printf("Implausible block height response from %s: \"%s\"\n", host, body.c_str());
            strlcpy(blockErr, "reply wasn't a block height", sizeof(blockErr));
        }
    } else {
        // The TLS library's own reason for the failure (e.g. a handshake
        // error or timeout) — HTTPClient only reports a generic -1.
        char tlsErr[100] = "";
        // The TLS reason only means something when the connection failed
        // (negative codes); after an HTTP error it's left over from before.
        if (!plainHttp && httpCode < 0) tlsReason(mempoolClient, tlsErr, sizeof(tlsErr));
        if (httpCode < 0) strlcpy(blockErr, "connection failed", sizeof(blockErr));
        else if (httpCode >= 300 && httpCode < 400) snprintf(blockErr, sizeof(blockErr), "HTTP %d redirect: try https", httpCode);
        else snprintf(blockErr, sizeof(blockErr), "HTTP %d", httpCode);
        Serial.printf("HTTP error fetching block height from %s: %d%s%s\n", host, httpCode,
                      tlsErr[0] ? " — TLS: " : "", tlsErr);
        Serial.printf("  WiFi.status()=%d (3=connected), IP=%s, RSSI=%d dBm, freeHeap=%u, largestBlock=%u\n",
                       WiFi.status(), WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                       ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        if (!strchr(host, ' ')) {   // a server's name (not "My node", which is an address)
            IPAddress resolvedIP;
            bool dnsOk = WiFi.hostByName(host, resolvedIP);
            Serial.printf("  DNS lookup for %s: %s%s\n", host,
                           dnsOk ? "OK -> " : "FAILED",
                           dnsOk ? resolvedIP.toString().c_str() : "");
        }
    }
    http.end();
    client.stop();   // make sure the TLS session and its buffers are freed now, not at the next request
    return blockHeight;
}
// ---------- Display ----------
void displayBitcoinInfo(const PriceSnapshot &snap, BlockAnim *anim) {
    dma_display->clearScreen();

    const BrandProfile &brand   = brandProfiles[currentBrandIndex];
    const Currency      currency = currencyOptions[currentCurrencyIndex];

    // Layout — every row's position lives here rather than as literals below.
    // Whole screen sits 2px higher than it originally did so the bottom (Sat)
    // row has a margin instead of sitting on the panel's last LED row, where
    // the enclosure bezel is most likely to clip it. Mirrors the simulator's
    // TITLE_Y / ROW1_Y / PRICE_Y / SAT_Y.
    const int16_t TITLE_Y = 4;
    const int16_t ROW1_Y  = 21;   // block chain + block height
    const int16_t PRICE_Y = 30;
    const int16_t SAT_Y   = 39;

    // Header — logo occupies x=0..21, title starts at x=24. Image-based
    // titles (katakana etc.) draw as a bitmap; ASCII titles print as text.
    if (brand.titleRegions != nullptr) {
        // Enlarged, logo-free rendering: each character trimmed to its own
        // content width and packed tightly, then scaled up as a whole —
        // fits noticeably larger than scaling the fixed-width cells would.
        // Centered across the full panel width (no logo on this profile).
        // Nudged down from pure vertical centering, right up against the
        // Block row's safe clearance (1px gap).
        uint16_t totalW = 0;
        for (uint8_t i = 0; i < brand.titleRegionCount; i++) {
            totalW += (uint16_t)(brand.titleRegions[i].w * brand.titleScale + 0.5f);
            totalW += (uint16_t)(brand.titleGap * brand.titleScale + 0.5f);
        }
        totalW -= (uint16_t)(brand.titleGap * brand.titleScale + 0.5f);  // no trailing gap
        int16_t dh = (int16_t)(brand.titleImage->height * brand.titleScale + 0.5f);
        int16_t dx = (96 - totalW) / 2;
        // Deliberately computed from 23 (the block row's original y), not
        // ROW1_Y — identical to the simulator. The lit katakana spans rows
        // 2-18, leaving 2px clear above the block row at y=21.
        int16_t dy = 23 - dh - 2;
        if (brand.titleBevel) {
            titleCanvas.fillScreen(0);
            drawScaledRegions(titleCanvas, brand.titleImage, brand.titleRegions, brand.titleRegionCount, dx, dy, brand.titleScale, brand.titleGap, 1);
            drawTitleBeveled();
        } else {
            drawScaledRegions(*dma_display, brand.titleImage, brand.titleRegions, brand.titleRegionCount, dx, dy, brand.titleScale, brand.titleGap, dma_display->color565(255, 255, 255));
        }
    } else if (brand.titleImage != nullptr) {
        // Native size, positioned in the title field alongside the logo —
        // y=6 matches "ITCOIN"'s text position exactly.
        dma_display->drawRGBBitmap(24, TITLE_Y, brand.titleImage->data, brand.titleImage->width, brand.titleImage->height);
    } else if (brand.titleLines != nullptr) {
        // Small multi-line title, each line at its own spot.
        dma_display->setTextSize(1);
        dma_display->setTextColor(dma_display->color565(brand.titleR, brand.titleG, brand.titleB));
        for (uint8_t i = 0; i < brand.titleLineCount; i++) {
            dma_display->setCursor(brand.titleLines[i].x, brand.titleLines[i].y);
            dma_display->print(brand.titleLines[i].text);
        }
    } else if (brand.titleBevel) {
        titleCanvas.fillScreen(0);
        titleCanvas.cp437(true);
        titleCanvas.setTextSize(2);
        titleCanvas.setTextColor(1);
        titleCanvas.setCursor(brand.titleX, TITLE_Y);
        titleCanvas.print(brand.title);
        drawTitleBeveled();
    } else {
        dma_display->setTextSize(2);
        dma_display->setTextColor(dma_display->color565(brand.titleR, brand.titleG, brand.titleB));
        dma_display->setCursor(brand.titleX, TITLE_Y);
        dma_display->print(brand.title);
    }

    // Block height — an icon in place of the word "Block" (see
    // drawBlockchainIcon), number aligned with the price/sats values.
    // setTextColor is set here because the block height number's print()
    // call below relies on it — the icon itself doesn't touch text color.
    dma_display->setTextSize(1);
    dma_display->setTextColor(dma_display->color565(255, 165, 30));  // amber
    const int16_t BLOCK_FIELD_X = 21; // stub lives at x=21..23
    const int16_t BLOCK_FIRST_DIGIT_X = 54;
    const int16_t BLOCK_Y = ROW1_Y;
    int16_t blockEndX;

    if (anim != nullptr) {
        // New-block arrival animation — ported from the simulator's
        // incomingActive branch (matrix_sim_v3_1.html), same four phases:
        // (1) the existing 3-block chain shifts left together, the oldest
        // one exiting through the row's left edge; (2) a new block flies
        // in from off-screen right in solid orange; (3) a 3px connector
        // grows in as it closes the final gap; (4) the new number slides
        // into the fixed digit position. The price and Sat rows share the
        // block number's right edge, and that edge stays pinned to the OLD
        // value's width for the whole animation, so neither row moves while
        // it plays — same as the simulator's staticSatBlockEndX.
        float p = (millis() - anim->startMs) / (float)BLOCK_ANIM_DURATION_MS;
        if (p > 1.0f) p = 1.0f;
        if (p < 0.0f) p = 0.0f;

        auto ease = [](float t) { return t * t * (3.0f - 2.0f * t); };

        float chainShiftP = p / 0.34f;
        if (chainShiftP > 1.0f) chainShiftP = 1.0f;
        int16_t chainShift = (int16_t)roundf(10.0f * ease(chainShiftP));

        int16_t oldFirstX  = 24 - chainShift;
        int16_t oldSecondX = 34 - chainShift;
        int16_t oldThirdX  = 44 - chainShift;

        drawClippedDots(oldFirstX - 3, BLOCK_Y + 3, 90, 90, 90, BLOCK_FIELD_X);
        drawBlockImageClipped(oldFirstX,  BLOCK_Y, 90, 90, 90, BLOCK_FIELD_X);
        drawBlockImageClipped(oldSecondX, BLOCK_Y, 90, 90, 90, BLOCK_FIELD_X);
        drawBlockImageClipped(oldThirdX,  BLOCK_Y, 170, 140, 90, BLOCK_FIELD_X);  // settles into the middle position once linked, so uses the midpoint tone
        drawClippedDots(oldFirstX + 7,  BLOCK_Y + 3, 90, 90, 90, BLOCK_FIELD_X);
        drawClippedDots(oldSecondX + 7, BLOCK_Y + 3, 90, 90, 90, BLOCK_FIELD_X);

        const int16_t INCOMING_START_X = 96 + 8; // a few pixels past the panel's right edge
        const int16_t INCOMING_END_X = BLOCK_FIRST_DIGIT_X - 10;
        float travelP = (p - 0.10f) / 0.60f;
        if (travelP < 0.0f) travelP = 0.0f;
        if (travelP > 1.0f) travelP = 1.0f;
        int16_t incomingX = (int16_t)roundf(INCOMING_START_X - (INCOMING_START_X - INCOMING_END_X) * ease(travelP));

        float linkP = (travelP - 0.82f) / 0.18f;
        if (linkP < 0.0f) linkP = 0.0f;
        if (linkP > 1.0f) linkP = 1.0f;
        // Solid orange for the entire flight — the same color it settles
        // into, no fade/transition. (Previously faded in from
        // white/near-white-green, matching the boot splash's "not yet
        // settled" look, but that read as a distracting color shift
        // rather than a clean arrival.)
        uint8_t blockR = 255, blockG = 165, blockB = 30;

        drawBlockImageClipped(incomingX, BLOCK_Y, blockR, blockG, blockB, BLOCK_FIELD_X);

        uint8_t linkLen = (uint8_t)roundf(3.0f * ease(linkP));
        drawClippedDots(oldThirdX + 7, BLOCK_Y + 3, blockR, blockG, blockB, BLOCK_FIELD_X, linkLen);

        // Runs to p=1.0, so the number emerges over the last 31% of
        // BLOCK_ANIM_DURATION_MS: a deliberate reveal rather than a quick
        // snap, with no idle time at the end of the animation.
        float numberP = (p - 0.69f) / 0.31f;
        if (numberP < 0.0f) numberP = 0.0f;
        if (numberP > 1.0f) numberP = 1.0f;
        if (numberP > 0.0f) {
            int16_t numberX = (int16_t)roundf(BLOCK_FIRST_DIGIT_X - 6.0f * (1.0f - ease(numberP)));
            dma_display->setTextColor(dma_display->color565(255, 165, 30));
            dma_display->setCursor(numberX, BLOCK_Y);
            dma_display->print(anim->newValue);
        }
        // Right edge for the price and Sat rows: the OLD value's width for
        // the entire animation (not getCursorX(), which would follow the
        // still-sliding number). If the digit count changes (999,999 ->
        // 1,000,000) the rows jump once, on the final static frame.
        blockEndX = BLOCK_FIRST_DIGIT_X + 6 * numDigits(anim->oldValue);
    } else {
        drawChainStub(BLOCK_FIELD_X, BLOCK_Y, 90, 90, 90);
        drawBlockchainIcon(BLOCK_FIELD_X + 3, BLOCK_Y, 90, 90, 90, 170, 140, 90, 255, 165, 30);  // oldest darkest, middle in between, newest full orange
        dma_display->setCursor(BLOCK_FIRST_DIGIT_X, BLOCK_Y);
        if (snap.blockHeight > 0) {
            dma_display->print(snap.blockHeight);
        } else {
            dma_display->print("ERR");
        }
        blockEndX = dma_display->getCursorX();
    }

    // Block height, price and Sat rows all share one RIGHT edge (blockEndX,
    // the end of the block number), so digits line up by place value down
    // the screen. The 24h change indicator ("▲2.3%") sits CHANGE_GAP px left
    // of the price, and only when the price itself is valid. Mirrors the
    // simulator's renderNormal().
    if (currency == Currency::JPY) {
        // BTC/JPY, shown man-scaled (万, units of 10,000) — e.g. ¥13,772,554
        // becomes "1377", matching how large yen amounts are natively read
        // in Japan.
        bool jpyOk = snap.priceJPY > 0.0f;
        uint16_t jpyColor = priceDirectionColor(snap.changePctJPY);
        bool jpyUp = isnan(snap.changePctJPY) || snap.changePctJPY >= 0.0f;
        // Fixed stack buffers instead of String temporaries: this function
        // runs every frame of the block animation (~28 times in 1.7s), and
        // each String is a small heap allocation — exactly the churn that
        // fragments the heap.
        char jpyStr[16];   // "¥1263" ('\x9D' is ¥ in the panel font)
        if (jpyOk) fmtJpyPrice(jpyStr, sizeof jpyStr, snap.priceJPY);
        else       strcpy(jpyStr, "\x9D" "ERR");
        int16_t priceW = 6 * (int16_t)strlen(jpyStr) + (jpyOk ? 8 : 0);   // ¥ + digits (+ 8px 万 glyph)
        int16_t priceX = blockEndX - priceW;
        bool showPct = jpyOk && !isnan(snap.changePctJPY);   // hidden when the 24h change is unknown

        // Price-change effect (see drawFxText()); the sweep covers the
        // whole row, from the arrow to the last digit.
        float fxP = jpyOk ? priceFxProgress() : -1.0f;
        int16_t rowX0 = showPct ? priceX - CHANGE_GAP - changeIndicatorWidth(snap.changePctJPY) : priceX;
        int16_t rowX1 = priceX + 6 * (int16_t)strlen(jpyStr);
        fxSweep.on = fxP >= 0.0f && currentPriceFx == FX_SWEEP;
        fxSweep.x0 = rowX0; fxSweep.x1 = rowX1;
        fxSweep.bx = rowX0 - 4 + (rowX1 - rowX0 + 8) * fxEase(fxP < 0 ? 0 : fxP);

        if (showPct) drawChangeIndicator(priceX, PRICE_Y, snap.changePctJPY, priceFx.oldPctJpy, priceFx.pctDirJpy, fxP);
        int16_t manX = drawFxText(jpyStr, priceFx.oldJpy, priceX, PRICE_Y,
                                  jpyOk ? (jpyUp ? 0 : 255) : 0, jpyOk ? (jpyUp ? 255 : 60) : 255, jpyOk ? (jpyUp ? 70 : 60) : 70,
                                  priceFx.dir, fxP);
        fxSweep.on = false;
        if (jpyOk) {
            // 万 icon, right after the last digit — positioned off the
            // end of the number (not a fixed x) so it stays glued to it
            // regardless of how many digits it has.
            if (manGlyph != nullptr) {
                drawBitmapTinted(manGlyph, manX, PRICE_Y, jpyColor);
            } else {
                drawPlaceholderBox(manX, PRICE_Y, 8, 8);
            }
        }

        // Sats per ¥100 — "Sat/¥100:" reads as sats-per-100-yen, matching
        // the corrected quantity/reference-amount order used on the USD
        // line's "Sat/$1:". Right-aligned to the shared right edge.
        char jpySatStr[24];
        fmtSatJpy(jpySatStr, sizeof jpySatStr, snap.satsPer100Yen);
        drawSatRow(jpySatStr, priceFx.oldSatJpy, priceFx.satDirJpy, blockEndX, SAT_Y, snap.satsPer100Yen > 0.0f);
    } else {
        // BTC/USD or BTC/EUR: the same layout, the full price. EUR swaps in
        // its own figures, the € sign and its own effect state.
        bool eur = currency == Currency::EUR;
        float fiatPrice  = eur ? snap.priceEUR     : snap.priceUSD;
        float fiatChange = eur ? snap.changePctEUR : snap.changePctUSD;
        float fiatSats   = eur ? snap.satsPerEUR   : snap.satsPerUSD;
        char *oldPrice = eur ? priceFx.oldEur    : priceFx.oldUsd;
        char *oldPct   = eur ? priceFx.oldPctEur : priceFx.oldPctUsd;
        char *oldSat   = eur ? priceFx.oldSatEur : priceFx.oldSatUsd;
        int8_t pctDir  = eur ? priceFx.pctDirEur : priceFx.pctDirUsd;
        int8_t satDir  = eur ? priceFx.satDirEur : priceFx.satDirUsd;
        bool usdOk = fiatPrice > 0.0f;
        bool usdUp = isnan(fiatChange) || fiatChange >= 0.0f;
        char usdStr[16];   // "$84509" / "€77450"
        if (usdOk) (eur ? fmtEurPrice : fmtUsdPrice)(usdStr, sizeof usdStr, fiatPrice);
        else       strcpy(usdStr, eur ? "\x9E" "ERR" : "$ERR");
        int16_t priceX = blockEndX - 6 * (int16_t)strlen(usdStr);
        bool showPct = usdOk && !isnan(fiatChange);   // hidden when the 24h change is unknown

        // Price-change effect (see drawFxText()); the sweep covers the
        // whole row, from the arrow to the last digit.
        float fxP = usdOk ? priceFxProgress() : -1.0f;
        int16_t rowX0 = showPct ? priceX - CHANGE_GAP - changeIndicatorWidth(fiatChange) : priceX;
        int16_t rowX1 = priceX + 6 * (int16_t)strlen(usdStr);
        fxSweep.on = fxP >= 0.0f && currentPriceFx == FX_SWEEP;
        fxSweep.x0 = rowX0; fxSweep.x1 = rowX1;
        fxSweep.bx = rowX0 - 4 + (rowX1 - rowX0 + 8) * fxEase(fxP < 0 ? 0 : fxP);

        if (showPct) drawChangeIndicator(priceX, PRICE_Y, fiatChange, oldPct, pctDir, fxP);
        drawFxText(usdStr, oldPrice, priceX, PRICE_Y,
                   usdOk ? (usdUp ? 0 : 255) : 0, usdOk ? (usdUp ? 255 : 60) : 255, usdOk ? (usdUp ? 70 : 60) : 70,
                   priceFx.dir, fxP);
        fxSweep.on = false;

        // Sats per dollar (euro) — right-aligned to the shared right edge.
        char usdSatStr[24];
        (eur ? fmtSatEur : fmtSatUsd)(usdSatStr, sizeof usdSatStr, fiatSats);
        drawSatRow(usdSatStr, oldSat, satDir, blockEndX, SAT_Y, fiatSats > 0.0f);
    }

    // Logo (overlaps the first letter of the title intentionally)
    // No placeholder box for profiles that intentionally have no logo (they
    // have a titleImage instead, so it's a deliberate design, not a stub) —
    // the placeholder is only for genuinely unfinished slots.
    if (brand.logo != nullptr) {
        // Transparent draw (black pixels skipped) rather than drawRGBBitmap,
        // which also paints the logo's black background — its 21px box
        // reaches x=21 and would erase the first dot of the chain stub.
        // Same as the simulator's drawBitmap565, which skips 0 pixels.
        for (int16_t ly = 0; ly < brand.logo->height; ly++)
            for (int16_t lx = 0; lx < brand.logo->width; lx++) {
                uint16_t px = brand.logo->data[ly * brand.logo->width + lx];
                if (px != 0) dma_display->drawPixel(brand.logoX + lx, brand.logoY + ly, px);
            }
    } else if (brand.titleImage == nullptr) {
        drawPlaceholderBox(1, 0, 21, 28);
    }
    dma_display->flipDMABuffer();
}

// ---------- Button handling (non-blocking, debounced) ----------
// Top button (brandButton, wired physically on top): short press = brightness
// up, long press = cycle brand.
// Bottom button (currencyButton): short press = brightness down, long press
// = cycle currency.
// Turns the display off (brightness 0) or back on at the saved brightness.
void setDisplayOff(bool off, const char *why) {
    if (off == displayOff) return;
    displayOff = off;
    dma_display->setBrightness8(off ? 0 : currentBrightness);
    Serial.printf("Display %s (%s)\n", off ? "off" : "on", why);
}

void adjustBrightness(int16_t delta) {
    // − at the lowest brightness turns the display off (any press brings it back).
    if (delta < 0 && currentBrightness <= BRIGHTNESS_MIN) { setDisplayOff(true, "button"); return; }
    // The next level up, or the last one below (delta only gives the direction).
    uint8_t next = delta > 0 ? BRIGHTNESS_MAX : BRIGHTNESS_MIN;
    for (uint8_t i = 0; i < NUM_BRIGHTNESS_LEVELS; i++) {
        uint8_t level = BRIGHTNESS_LEVELS[i];
        if (delta > 0 && level > currentBrightness) { next = level; break; }
        if (delta < 0 && level < currentBrightness) next = level;
    }
    currentBrightness = next;

    dma_display->setBrightness8(currentBrightness);
    // Saved once the presses stop (like the web slider), rather than a flash
    // write for every step.
    brightnessSaveDueMs = millis() + BRIGHTNESS_SAVE_DELAY_MS;
    if (brightnessSaveDueMs == 0) brightnessSaveDueMs = 1;

    // Brightness is a display-driver setting, not a content change — no
    // need to rebuild the frame, just let it show at the new level.
}

// Writes a brightness change that's still waiting for its save delay.
void saveBrightnessNow() {
    if (brightnessSaveDueMs == 0) return;
    preferences.putUChar("brightness", currentBrightness);
    brightnessSaveDueMs = 0;
    Serial.printf("Brightness set to %u\n", currentBrightness);
}

// Shared by the both-buttons hold and the web page's Reboot button.
void rebootNow(const char *reason) {
    Serial.printf("%s — rebooting...\n", reason);
    saveBrightnessNow();   // a change made just before the reboot isn't lost
    setDisplayOff(false, "rebooting");
    dma_display->clearScreen();
    dma_display->setTextSize(1);
    dma_display->setTextColor(dma_display->color565(255, 60, 60));
    dma_display->setCursor(14, 20);
    dma_display->print("REBOOTING");
    dma_display->flipDMABuffer(); // without this the message stays in the back buffer and is never actually seen
    delay(300); // let the message actually show before the restart cuts the display driver off
    ESP.restart();
}

// Carries out whatever the web control page asked for since the last pass
// (see the web hand-off globals). Runs on loop()'s side, so drawing and
// saving settings here is as safe as it is for the physical buttons.
void processWebCommands() {
    serveWeb();

    uint32_t rebootAt = webRebootRequestedMs.load();
    if (rebootAt != 0 && millis() - rebootAt >= 500) {   // gives the page's request time to get its reply
        rebootNow("Reboot requested from the web page");
    }

    int32_t b = webPendingBrightness.exchange(-1);
    if (b >= 0 && ((uint8_t)b != currentBrightness || displayOff)) {
        bool changed = (uint8_t)b != currentBrightness;
        currentBrightness = (uint8_t)b;
        if (displayOff) setDisplayOff(false, "web brightness");   // moving the slider turns it back on
        else dma_display->setBrightness8(currentBrightness);
        if (changed) {
            brightnessSaveDueMs = millis() + BRIGHTNESS_SAVE_DELAY_MS;
            if (brightnessSaveDueMs == 0) brightnessSaveDueMs = 1;
        }
    }
    int32_t d = webPendingDisplay.exchange(-1);
    if (d >= 0) setDisplayOff(d == 0, "web");
    if (brightnessSaveDueMs != 0 && (long)(millis() - brightnessSaveDueMs) >= 0) {
        saveBrightnessNow();
    }

    bool redraw = false;
    int32_t br = webPendingBrand.exchange(-1);
    if (br >= 0 && br < NUM_BRAND_PROFILES && (uint8_t)br != currentBrandIndex) {
        currentBrandIndex = (uint8_t)br;
        preferences.putUChar("brand", currentBrandIndex);
        Serial.printf("Theme switched to index %u (web)\n", currentBrandIndex);
        redraw = true;
    }
    int32_t fx = webPendingPriceFx.exchange(-1);
    if (fx >= 0 && fx < NUM_PRICE_FX && (uint8_t)fx != currentPriceFx) {
        currentPriceFx = (uint8_t)fx;
        priceFx.active = false;   // a new choice applies from the next price change
        preferences.putUChar("priceFx", currentPriceFx);
        Serial.printf("Price effect set to %s (web)\n", priceFxNames[currentPriceFx]);
    }
    // Data sources: saved, and a check is made straight away with the new
    // setting rather than waiting for the next one.
    int32_t ps = webPendingPriceSrc.exchange(-1);
    if (ps >= 0 && ps < NUM_PRICE_SRC && (uint8_t)ps != currentPriceSrc) {
        currentPriceSrc = (uint8_t)ps;
        preferences.putUChar("priceSrc", currentPriceSrc);
        coingeckoBackoffUntilMs = 0;   // a fresh choice gets a fresh try
        coingeckoRefusals = 0;
        lastPriceCheckMs = 0;          // check the price on the next pass
        Serial.printf("Price source set to %s (web)\n", priceSrcNames[currentPriceSrc]);
    }
    if (webPendingNodeUrlSet.exchange(false) && webPendingNodeUrl != nodeUrl) {
        nodeUrl = webPendingNodeUrl;
        preferences.putString("nodeUrl", nodeUrl);
        nodeOk = -1;
        lastBlockCheckMs = 0;          // try it on the next pass
        Serial.printf("Node address set to \"%s\" (web)\n", nodeUrl.c_str());
    }
    int32_t bs = webPendingBlockSrc.exchange(-1);
    if (bs >= 0 && bs < NUM_BLOCK_SRC && (uint8_t)bs != currentBlockSrc) {
        currentBlockSrc = (uint8_t)bs;
        preferences.putUChar("blockSrc", currentBlockSrc);
        lastBlockCheckMs = 0;          // check the block height on the next pass
        nodeOk = -1;
        Serial.printf("Block height source set to %s (web)\n", blockSrcNames[currentBlockSrc]);
    }
    int32_t iv = webPendingInterval.exchange(-1);
    if (iv >= 0 && iv < NUM_INTERVALS && (uint8_t)iv != currentInterval) {
        currentInterval = (uint8_t)iv;
        preferences.putUChar("interval", currentInterval);
        Serial.printf("Checking every %s (web)\n", intervalNames[currentInterval]);
    }
    int32_t c = webPendingCurrency.exchange(-1);
    if (c >= 0 && c < NUM_CURRENCY_OPTIONS && (uint8_t)c != currentCurrencyIndex) {
        currentCurrencyIndex = (uint8_t)c;
        preferences.putUChar("currency", (uint8_t)currencyOptions[currentCurrencyIndex]);
        Serial.printf("Currency switched to %s (web)\n", currencyCode(currencyOptions[currentCurrencyIndex]));
        redraw = true;
    }
    if (redraw) displayBitcoinInfo(currentSnapshot());
}

void handleButtons() {
    // Web page requests are picked up here too, since everywhere that waits
    // for a while (the fetch retry, the new-block animation) already calls
    // handleButtons() — so the page stays as responsive as the buttons.
    processWebCommands();

#if !HAS_BUTTONS
    return;   // no buttons fitted — the web control page is the only control
#endif

    // Both buttons are always polled first, every pass, regardless of what
    // happens below — this keeps each button's own debounce state machine
    // correctly tracking presses/releases even while a combo-hold is in
    // progress and we're choosing not to act on the individual events.
    ButtonEvent brandEvent = brandButton.poll();
    ButtonEvent currencyEvent = currencyButton.poll();

    bool bothHeld = (brandButton.stableState == LOW && currencyButton.stableState == LOW);

    if (bothHeld) {
        if (!rebootHoldActive) {
            rebootHoldActive = true;
            rebootHoldStartMs = millis();
        } else if (millis() - rebootHoldStartMs >= REBOOT_HOLD_MS) {
            rebootNow("Both buttons held");
        }
        // Swallow both buttons' events for this pass while they're held
        // together — otherwise each one's own long-press would still fire
        // independently at LONG_PRESS_THRESHOLD_MS (cycling brand AND
        // currency at once), which isn't what a deliberate two-button
        // hold is going for.
        return;
    }
    rebootHoldActive = false;

    // While the display is off, a press (short or long, either button) only
    // turns it back on.
    if (displayOff) {
        if (brandEvent != ButtonEvent::NONE || currencyEvent != ButtonEvent::NONE) setDisplayOff(false, "button");
        return;
    }

    if (brandEvent == ButtonEvent::SHORT_PRESS) {
        adjustBrightness(+1);
    } else if (brandEvent == ButtonEvent::LONG_PRESS) {
        currentBrandIndex = (currentBrandIndex + 1) % NUM_BRAND_PROFILES;
        preferences.putUChar("brand", currentBrandIndex);
        Serial.printf("Theme switched to index %u\n", currentBrandIndex);

        // Redraw immediately with the last known data rather than
        // waiting up to UPDATE_INTERVAL_MS for the next refresh.
        displayBitcoinInfo(currentSnapshot());
    }

    if (currencyEvent == ButtonEvent::SHORT_PRESS) {
        adjustBrightness(-1);
    } else if (currencyEvent == ButtonEvent::LONG_PRESS) {
        currentCurrencyIndex = (currentCurrencyIndex + 1) % NUM_CURRENCY_OPTIONS;
        preferences.putUChar("currency", (uint8_t)currencyOptions[currentCurrencyIndex]);
        Serial.printf("Currency switched to %s\n", currencyCode(currencyOptions[currentCurrencyIndex]));

        displayBitcoinInfo(currentSnapshot());
    }
}

// Fetches fresh data and updates the cached last* values (used when just
// redrawing on a brand/currency button press, without a new fetch) —
// but doesn't touch the display itself. Split out from refreshBitcoinData()
// specifically so setup() can fetch the first data, then wait out the
// splash's minimum duration, THEN display it — rather than displaying
// immediately the instant data happens to be ready.
// ---------- Keeping the splash animated during the first fetch ----------
// The first price/block fetch runs in setup() itself — the main task, whose
// large stack is already set aside for exactly this. Meanwhile a 50 ms
// periodic timer keeps drawing the rain, so the splash never freezes while
// the HTTPS requests are in flight.
//
// The timer callback runs in the ESP32's own always-present timer task, so
// this costs no extra memory at all. (Running the fetch in a task of its
// own would take a 10 KB stack from the same free memory the secure
// connections need, and the block-height fetches would fail with
// "memory allocation failed".) Drawing a splash frame takes a few ms,
// comfortably short for a timer callback.
//
// Only the callback touches the display while the timer runs; setup() does
// the network work and doesn't draw until the timer has been stopped.
void splashTimerCallback(void *) {
    splashCallbackBusy = true;
    drawSplashFrame();
    splashCallbackBusy = false;
}

void startSplashRenderer() {
    esp_timer_create_args_t args = {};
    args.callback = splashTimerCallback;
    args.name = "splashDraw";
    if (esp_timer_create(&args, &splashTimer) != ESP_OK || esp_timer_start_periodic(splashTimer, 50000) != ESP_OK) {
        Serial.println("Couldn't start the splash timer — the splash will pause during the first fetch");
        if (splashTimer) { esp_timer_delete(splashTimer); splashTimer = nullptr; }
    }
}

void stopSplashRenderer() {
    if (!splashTimer) return;
    esp_timer_stop(splashTimer);
    while (splashCallbackBusy) delay(1);   // let a frame that's mid-draw finish
    esp_timer_delete(splashTimer);
    splashTimer = nullptr;
}

// allowUi = false during the boot splash: the retry wait then just sleeps
// instead of polling the buttons and web page, which can draw on the panel
// while the splash timer is drawing it.
PriceSnapshot fetchAndCacheBitcoinData(bool allowUi) {
    float price = 0.0f, priceJPY = 0.0f, priceEUR = 0.0f;
    float usdChange = NAN, jpyChange = NAN, eurChange = NAN;
    // Each is only fetched when it's due: the price at the "Check every"
    // interval, the block height every minute (see BLOCK_CHECK_MS).
    bool wantPrice = priceCheckDue(), wantBlock = blockCheckDue();
    bool priceOk = false;
    uint32_t blockHeight = 0;
    // How long each took: a failure that took the full timeout (a server not
    // answering at all) isn't repeated straight away — repeating it would
    // just freeze the buttons and web page for another round of timeouts.
    // Quick failures (refused, a dropped packet) get the one retry below.
    unsigned long priceMs = 0, blockMs = 0;
    if (wantPrice) { lastPriceCheckMs = millis(); priceOk = fetchBitcoinPrices(price, priceJPY, usdChange, jpyChange, priceEUR, eurChange); priceMs = millis() - lastPriceCheckMs; }
    if (wantBlock) { lastBlockCheckMs = millis(); blockHeight = fetchBlockHeight(); blockMs = millis() - lastBlockCheckMs; }
    bool retryPrice = wantPrice && !priceOk && priceMs < HTTP_TIMEOUT_MS;
    bool retryBlock = wantBlock && blockHeight == 0 && blockMs < HTTP_TIMEOUT_MS;
    if (wantPrice && !priceOk && !retryPrice) Serial.printf("Price check timed out (%lu ms) — not retrying until the next check\n", priceMs);
    if (wantBlock && blockHeight == 0 && !retryBlock) Serial.printf("Block check timed out (%lu ms) — not retrying until the next check\n", blockMs);

    // One quick retry on failure rather than waiting a full UPDATE_INTERVAL_MS
    // for the next scheduled cycle — papers over brief transient blips (a
    // dropped packet, a momentary power dip) without masking a genuinely
    // broken connection, since it only retries once before giving up.
    if (retryPrice || retryBlock) {
        // Was a blind delay(1500) — replaced with a polling wait so button
        // presses still register during this gap. The actual fetch calls
        // below are still fully blocking (HTTPClient's GET() has no way to
        // yield mid-request), so this doesn't fully solve button
        // unresponsiveness during a failure streak — a genuinely
        // unresponsive-free fix would mean rearchitecting these as
        // non-blocking requests, a much bigger change than this — but it
        // does remove one meaningful chunk (up to 1.5s per retry) of the
        // dead time a failing cycle spends unable to notice a press.
        unsigned long waitStart = millis();
        while (millis() - waitStart < 1500) {
            if (allowUi) handleButtons();
            delay(50);
        }
        if (retryPrice) priceOk = fetchBitcoinPrices(price, priceJPY, usdChange, jpyChange, priceEUR, eurChange);
        if (retryBlock) blockHeight = fetchBlockHeight();
    }

    // Only successful results replace the cache. A failure leaves the last
    // good values in place (see currentSnapshot() for when they're shown).
    if (priceOk) {
        lastPrice         = price;
        lastSatoshis      = calculateSatoshisPerDollar(price);
        lastPriceJPY      = priceJPY;
        lastSatsPer100Yen = calculateSatoshisPer100Yen(priceJPY);
        lastChangePctUSD  = usdChange;
        lastChangePctJPY  = jpyChange;
        lastPriceEUR      = priceEUR;
        lastSatsPerEUR    = calculateSatoshisPerDollar(priceEUR);   // same sum: sats per 1 unit
        lastChangePctEUR  = eurChange;
        lastGoodPriceMs   = millis();
        havePriceData     = true;
    } else if (wantPrice && havePriceData) {
        Serial.printf("Price fetch failed — keeping cached price from %lus ago\n", (millis() - lastGoodPriceMs) / 1000UL);
    }
    if (blockHeight > 0) {
        lastBlockHeight = blockHeight;
        lastGoodBlockMs = millis();
        haveBlockData   = true;
    } else if (wantBlock && haveBlockData) {
        Serial.printf("Block height fetch failed — keeping cached height from %lus ago\n", (millis() - lastGoodBlockMs) / 1000UL);
    }

    return currentSnapshot();
}

// What the screen should show right now: the cached values while they're
// fresh, or 0 (drawn as ERR) once they're older than DATA_STALE_AFTER_MS
// or if there's never been a successful fetch. Price-derived fields go
// stale together; block height goes stale on its own. Used for every
// redraw — scheduled refreshes and button presses alike.
PriceSnapshot currentSnapshot() {
    unsigned long now = millis();
    bool priceFresh = havePriceData && (now - lastGoodPriceMs <= DATA_STALE_AFTER_MS);
    bool blockFresh = haveBlockData && (now - lastGoodBlockMs <= BLOCK_STALE_AFTER_MS);
    return {
        priceFresh ? lastPrice         : 0.0f,
        priceFresh ? lastSatoshis      : 0.0f,
        priceFresh ? lastPriceJPY      : 0.0f,
        priceFresh ? lastSatsPer100Yen : 0.0f,
        blockFresh ? lastBlockHeight   : 0,
        priceFresh ? lastChangePctUSD  : NAN,
        priceFresh ? lastChangePctJPY  : NAN,
        priceFresh ? lastPriceEUR      : 0.0f,
        priceFresh ? lastSatsPerEUR    : 0.0f,
        priceFresh ? lastChangePctEUR  : NAN,
    };
}

// Plays the new-block arrival animation (see displayBitcoinInfo()'s
// anim branch) for BLOCK_ANIM_DURATION_MS, then leaves the display on
// the final, fully-resolved static frame. Blocking, like the boot splash
// — buttons are still polled each frame so a press during the ~1.3s
// animation isn't simply dropped, even though the animation itself can't
// be interrupted mid-flight.
void playNewBlockAnimation(uint32_t oldValue, uint32_t newValue, PriceSnapshot snap) {
    BlockAnim anim = { oldValue, newValue, millis() };
    while (millis() - anim.startMs < BLOCK_ANIM_DURATION_MS) {
        handleButtons();
        displayBitcoinInfo(snap, &anim);
        delay(60);
    }
    // Final frame, fully resolved — snap.blockHeight should already equal
    // newValue (this is called right after a fetch that found it), but
    // set it explicitly so the static rendering is correct even if that
    // ever isn't the case.
    snap.blockHeight = newValue;
    displayBitcoinInfo(snap);
}

// Arms the price-change effect if the price or 24h % text is about to
// change. Nothing plays after a failed fetch or when there was no previous
// value to animate from (first data, or coming back from ERR).
void startPriceFx(const PriceSnapshot &before, const PriceSnapshot &after) {
    priceFx.active = false;
    if (currentPriceFx == FX_OFF || before.priceUSD <= 0.0f || after.priceUSD <= 0.0f) return;
    fmtUsdPrice(priceFx.oldUsd, sizeof priceFx.oldUsd, before.priceUSD);
    fmtJpyPrice(priceFx.oldJpy, sizeof priceFx.oldJpy, before.priceJPY);
    fmtPct(priceFx.oldPctUsd, sizeof priceFx.oldPctUsd, before.changePctUSD);
    fmtPct(priceFx.oldPctJpy, sizeof priceFx.oldPctJpy, before.changePctJPY);
    fmtSatUsd(priceFx.oldSatUsd, sizeof priceFx.oldSatUsd, before.satsPerUSD);
    fmtSatJpy(priceFx.oldSatJpy, sizeof priceFx.oldSatJpy, before.satsPer100Yen);
    fmtEurPrice(priceFx.oldEur, sizeof priceFx.oldEur, before.priceEUR);
    fmtPct(priceFx.oldPctEur, sizeof priceFx.oldPctEur, before.changePctEUR);
    fmtSatEur(priceFx.oldSatEur, sizeof priceFx.oldSatEur, before.satsPerEUR);
    char nu[16], nj[16], pu[12], pj[12], su[24], sj[24], ne[16], pe[12], se[24];
    fmtEurPrice(ne, sizeof ne, after.priceEUR);
    fmtPct(pe, sizeof pe, after.changePctEUR);
    fmtSatEur(se, sizeof se, after.satsPerEUR);
    fmtUsdPrice(nu, sizeof nu, after.priceUSD);
    fmtJpyPrice(nj, sizeof nj, after.priceJPY);
    fmtPct(pu, sizeof pu, after.changePctUSD);
    fmtPct(pj, sizeof pj, after.changePctJPY);
    fmtSatUsd(su, sizeof su, after.satsPerUSD);
    fmtSatJpy(sj, sizeof sj, after.satsPer100Yen);
    if (!strcmp(nu, priceFx.oldUsd) && !strcmp(nj, priceFx.oldJpy) &&
        !strcmp(pu, priceFx.oldPctUsd) && !strcmp(pj, priceFx.oldPctJpy) &&
        !strcmp(su, priceFx.oldSatUsd) && !strcmp(sj, priceFx.oldSatJpy) &&
        !strcmp(ne, priceFx.oldEur) && !strcmp(pe, priceFx.oldPctEur) && !strcmp(se, priceFx.oldSatEur)) return;   // nothing visible changed
    priceFx.satDirUsd = (uint32_t)after.satsPerUSD >= (uint32_t)before.satsPerUSD ? 1 : -1;
    priceFx.satDirJpy = (uint32_t)after.satsPer100Yen >= (uint32_t)before.satsPer100Yen ? 1 : -1;
    priceFx.satDirEur = (uint32_t)after.satsPerEUR >= (uint32_t)before.satsPerEUR ? 1 : -1;
    priceFx.dir = after.priceUSD >= before.priceUSD ? 1 : -1;
    priceFx.pctDirUsd = (isnan(before.changePctUSD) || fabsf(after.changePctUSD) >= fabsf(before.changePctUSD)) ? 1 : -1;
    priceFx.pctDirJpy = (isnan(before.changePctJPY) || fabsf(after.changePctJPY) >= fabsf(before.changePctJPY)) ? 1 : -1;
    priceFx.pctDirEur = (isnan(before.changePctEUR) || fabsf(after.changePctEUR) >= fabsf(before.changePctEUR)) ? 1 : -1;
    priceFx.startMs = millis();
    priceFx.active = true;
}

// Plays the price-change effect to the end (under a second), then leaves
// the final frame up. Blocking like the new-block animation, with buttons
// and the web page still served each frame.
void playPriceFx(const PriceSnapshot &snap) {
    while (priceFx.active) {
        handleButtons();
        displayBitcoinInfo(snap);   // priceFxProgress() ends the effect when its time is up
        delay(30);
    }
    displayBitcoinInfo(snap);
}

// Regular periodic refresh (called from loop()) — fetch, cache, and
// display immediately, no splash/minimum-duration logic involved.
void refreshBitcoinData() {
    uint32_t previousBlockHeight = lastBlockHeight;  // last GOOD height (failures never overwrite it), captured before this fetch can update it
    PriceSnapshot before = currentSnapshot();        // what the screen shows now, for the price-change effect
    PriceSnapshot snap = fetchAndCacheBitcoinData(true);
    startPriceFx(before, snap);

    // previousBlockHeight > 0 excludes the very first successful fetch
    // ever (where it's still at its startup default) — that's a normal
    // first display, not a new block arriving. Also requires a valid
    // price fetch (matching the simulator's equivalent !err guard) —
    // if only the block height succeeded, this shows the normal static
    // frame (with its own independent ERR handling on the price row)
    // rather than animating into a screen with a broken price line.
    if (snap.blockHeight > previousBlockHeight && previousBlockHeight > 0 && snap.priceUSD > 0.0f) {
        playNewBlockAnimation(previousBlockHeight, snap.blockHeight, snap);   // the price effect plays along within it
    } else if (priceFx.active) {
        playPriceFx(snap);
    } else {
        displayBitcoinInfo(snap);
    }
    lastUpdateMs = millis();
}


// ---------- Main loop (non-blocking) ----------
void loop() {
    // In AP mode only the setup page needs serving (OTA isn't started until
    // we're on the home WiFi network, in RUNNING/RECONNECTING state).
    if (currentState == DeviceState::AP_MODE) {
        dnsServer.processNextRequest();
        serveWeb();
        pollWifiScan();
        retrySavedNetworkFromSetup();
        delay(2);
        return;
    }

    if (otaInitialized) ArduinoOTA.handle();

    handleButtons();  // cheap poll — fine to run every loop pass
    pollWifiScan();     // the web page's Wi-Fi card
    processWifiJoin();

    unsigned long now = millis();

    // Wi-Fi lost: noticed within a few seconds (not only when a check is
    // due), then retried without blocking — the buttons and display keep
    // working meanwhile. See WIFI_RETRY_FIRST_MS.
    if (currentState == DeviceState::RUNNING) {
        if (WiFi.status() == WL_CONNECTED) wifiLostSinceMs = 0;
        else if (wifiLostSinceMs == 0) wifiLostSinceMs = now ? now : 1;
        else if (now - wifiLostSinceMs >= WIFI_LOST_GRACE_MS) {
            Serial.println("WiFi disconnected. Reconnecting...");
            WiFi.reconnect();
            reconnectStartMs = now;
            reconnectDelayMs = WIFI_RETRY_FIRST_MS;
            lastOfflineRedrawMs = now;
            currentState = DeviceState::RECONNECTING;
        }
    }
    if (currentState == DeviceState::RECONNECTING) {
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("Reconnected after %lus.\n", (now - wifiLostSinceMs) / 1000UL);
            currentState = DeviceState::RUNNING;
            wifiLostSinceMs = 0;
            setupOTA();  // re-arm OTA in case the IP/mDNS state went stale
            lastPriceCheckMs = lastBlockCheckMs = 0;   // fresh data straight away
        } else if (now - wifiLostSinceMs >= WIFI_FALLBACK_SETUP_MS) {
            Serial.printf("No WiFi for %lus — switching to Wi-Fi setup mode\n", (now - wifiLostSinceMs) / 1000UL);
            saveBrightnessNow();
            startAccessPoint();   // keeps retrying the saved network (see retrySavedNetworkFromSetup())
            return;
        } else {
            if (now - reconnectStartMs >= reconnectDelayMs) {
                // A fresh attempt rather than waiting on a stuck one.
                Serial.printf("Still no WiFi (%lus) — trying again\n", (now - wifiLostSinceMs) / 1000UL);
                WiFi.disconnect();
                WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
                WiFi.setSleep(false);
                reconnectStartMs = now;
                reconnectDelayMs = reconnectDelayMs * 2 > WIFI_RETRY_MAX_MS ? WIFI_RETRY_MAX_MS : reconnectDelayMs * 2;
            }
            // Keep the screen honest while offline: once the data is too old
            // it turns to ERR, as it would after failed checks.
            if (now - lastOfflineRedrawMs >= BLOCK_CHECK_MS) {
                lastOfflineRedrawMs = now;
                displayBitcoinInfo(currentSnapshot());
            }
        }
        return;
    }

    // Refresh data on first run (lastUpdateMs == 0) or after interval. In
    // practice lastUpdateMs is already set directly at the end of setup()
    // before loop() ever runs, but the == 0 check stays here as a safety
    // net in case that ever changes.
    if (lastUpdateMs == 0 || priceCheckDue() || blockCheckDue()) {
        if (WiFi.status() != WL_CONNECTED) return;   // handled above once it's been down a few seconds
        if (scanRunning) return;                     // a Wi-Fi scan takes a few seconds; fetch after it

        refreshBitcoinData();
    }
    // loop() returns immediately, so buttons and the web page stay responsive
}
