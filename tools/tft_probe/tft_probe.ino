/**
 * TFT Probe — XW-TS0526 / UN2572 Serial Discovery
 * ===================================================
 * ATmega2560 sketch to reverse-engineer the UART protocol of a
 * Firerose/ELUX vape module TFT display.
 *
 * WIRING — connect in this exact order before uploading:
 * -------------------------------------------------------
 *
 *  LEVEL SHIFTER (BSS138 module):
 *  ┌─────────────────────────────────────────────────────────┐
 *  │  HV side (5V)          │  LV side (3.3V)               │
 *  │  HV  ── P3 5V pin      │  LV  ── P3 3.3V pin           │
 *  │  GND ── P3 GND         │  GND ── P3 GND                │
 *  │  HV1 ── P3 Pin 2 (TX)  │  LV1 ──┐                      │
 *  │  HV2 ── P3 Pin 3 (RX)  │  LV2 ──┴── U_TX.RX on display │
 *  └─────────────────────────────────────────────────────────┘
 *
 *  DISPLAY PADS (top to bottom as identified):
 *  ┌──────────────┬────────────────────────────────────────┐
 *  │ Display pad  │ Connect to                             │
 *  ├──────────────┼────────────────────────────────────────┤
 *  │ VCC          │ P3 3.3V pin  (NOT 5V)                  │
 *  │ U_TX.RX      │ Level shifter LV1 + LV2 bridged        │
 *  │ TFT_DET      │ P3 GND  (ties it low permanently)      │
 *  │ GND          │ P3 GND                                 │
 *  └──────────────┴────────────────────────────────────────┘
 *
 *  TX vs RX — does it matter?
 *  Pin 2 is TX (ATmega sends data OUT to display).
 *  Pin 3 is RX (ATmega reads data IN from display).
 *  Both go to the same single U_TX.RX pad on the display.
 *  If you swap them the sketch will still work — the only
 *  difference is which wire is logically "sending" vs "receiving"
 *  inside the ATmega. Since both wires land on the same display
 *  pad, swapping them has no effect on the physical circuit.
 *
 *  NOTE ON TFT_DET:
 *  Earlier notes said wire TFT_DET to a GPIO pin. Simpler to
 *  just tie it to GND permanently — this keeps the module awake
 *  the whole time without needing a pin toggle in code.
 *
 * HOW IT WORKS:
 *   Phase 1 — Passive listen: Sweeps baud rates 2400→115200.
 *              At each rate listens 800ms for bootloader output,
 *              logs all bytes as hex+ASCII.
 *   Phase 2 — Active probe: At the most promising baud rate sends
 *              common protocol init sequences (AT commands, DMT/DWIN
 *              headers, raw sync bytes) and logs any responses.
 *   Phase 3 — Interactive: optional hex passthrough mode.
 *              Set INTERACTIVE_MS > 0 to enable.
 *
 * Read output in Arduino Serial Monitor at 115200 baud.
 */

#include <Arduino.h>
#include <SoftwareSerial.h>

// ── Pin config ───────────────────────────────────────────────────────────────
//
//  Pin 2 = TX — ATmega sends data to the display
//  Pin 3 = RX — ATmega receives data from the display
//  Both wires land on the same U_TX.RX pad on the display board.
//
#define TFT_TX_PIN   2    // P3 pin 2  → HV1 → LV1 → U_TX.RX pad
#define TFT_RX_PIN   3    // P3 pin 3  → HV2 → LV2 → U_TX.RX pad (same pad)

// TFT_DET is now tied to GND on the board — no pin needed in code.

#define DEBUG_SERIAL    Serial    // USB serial for output
#define DEBUG_BAUD      115200

// Baud rates to sweep
const uint32_t BAUD_LIST[] = {
    9600, 19200, 38400, 57600, 115200, 4800, 2400, 76800
};
const uint8_t BAUD_COUNT = sizeof(BAUD_LIST) / sizeof(BAUD_LIST[0]);

// How long to listen at each baud rate during sweep
#define LISTEN_MS       800

// Set to e.g. 60000 to enable interactive hex passthrough after probing
#define INTERACTIVE_MS  0


// ── Globals ──────────────────────────────────────────────────────────────────
SoftwareSerial TFT_SERIAL(TFT_RX_PIN, TFT_TX_PIN);  // RX, TX
int      best_baud_idx   = -1;
uint16_t best_byte_count = 0;


// ── Helpers ──────────────────────────────────────────────────────────────────

void print_hex_ascii(const uint8_t* buf, uint16_t len, uint32_t baud) {
    if (len == 0) return;
    if (baud > 0) {
        DEBUG_SERIAL.print(F("["));
        DEBUG_SERIAL.print(baud);
        DEBUG_SERIAL.print(F("] "));
    }
    DEBUG_SERIAL.print(len);
    DEBUG_SERIAL.print(F(" bytes │ HEX: "));
    for (uint16_t i = 0; i < len; i++) {
        if (buf[i] < 0x10) DEBUG_SERIAL.print('0');
        DEBUG_SERIAL.print(buf[i], HEX);
        DEBUG_SERIAL.print(' ');
    }
    DEBUG_SERIAL.print(F("│ ASCII: "));
    for (uint16_t i = 0; i < len; i++) {
        char c = (char)buf[i];
        DEBUG_SERIAL.print(isPrintable(c) ? c : '.');
    }
    DEBUG_SERIAL.println();
}

void send_probe(const uint8_t* data, uint8_t len, const char* label) {
    DEBUG_SERIAL.print(F("  → "));
    DEBUG_SERIAL.print(label);
    DEBUG_SERIAL.print(F(": "));
    for (uint8_t i = 0; i < len; i++) {
        if (data[i] < 0x10) DEBUG_SERIAL.print('0');
        DEBUG_SERIAL.print(data[i], HEX);
        DEBUG_SERIAL.print(' ');
    }
    DEBUG_SERIAL.println();

    TFT_SERIAL.write(data, len);

    uint8_t  rbuf[64];
    uint8_t  rlen = 0;
    uint32_t t    = millis();
    while (millis() - t < 300 && rlen < sizeof(rbuf)) {
        if (TFT_SERIAL.available())
            rbuf[rlen++] = TFT_SERIAL.read();
    }
    if (rlen > 0) {
        DEBUG_SERIAL.print(F("  ← "));
        print_hex_ascii(rbuf, rlen, 0);
    } else {
        DEBUG_SERIAL.println(F("  ← (no response)"));
    }
}


// ── Phase 1: Passive baud-rate sweep ─────────────────────────────────────────

void phase1_passive_sweep() {
    DEBUG_SERIAL.println(F("\n╔══════════════════════════════════════════╗"));
    DEBUG_SERIAL.println(F("║  PHASE 1 — Passive baud-rate sweep       ║"));
    DEBUG_SERIAL.println(F("╚══════════════════════════════════════════╝"));
    DEBUG_SERIAL.println(F("Listening at each baud rate for bootloader output."));
    DEBUG_SERIAL.println();

    for (uint8_t bi = 0; bi < BAUD_COUNT; bi++) {
        uint32_t baud = BAUD_LIST[bi];

        DEBUG_SERIAL.print(F("Testing "));
        DEBUG_SERIAL.print(baud);
        DEBUG_SERIAL.print(F(" baud ... "));

        TFT_SERIAL.begin(baud);
        while (TFT_SERIAL.available()) TFT_SERIAL.read();  // flush

        uint8_t  buf[128];
        uint16_t len = 0;
        uint32_t t   = millis();
        while (millis() - t < LISTEN_MS && len < sizeof(buf)) {
            if (TFT_SERIAL.available())
                buf[len++] = TFT_SERIAL.read();
        }

        TFT_SERIAL.end();

        if (len > 0) {
            DEBUG_SERIAL.print(len);
            DEBUG_SERIAL.println(F(" bytes received!"));
            print_hex_ascii(buf, len, baud);
            if (len > best_byte_count) {
                best_byte_count = len;
                best_baud_idx   = bi;
            }
        } else {
            DEBUG_SERIAL.println(F("(silence)"));
        }

        delay(200);
    }

    DEBUG_SERIAL.println();
    if (best_baud_idx >= 0) {
        DEBUG_SERIAL.print(F("★ Best candidate: "));
        DEBUG_SERIAL.print(BAUD_LIST[best_baud_idx]);
        DEBUG_SERIAL.print(F(" baud ("));
        DEBUG_SERIAL.print(best_byte_count);
        DEBUG_SERIAL.println(F(" bytes)"));
    } else {
        DEBUG_SERIAL.println(F("✗ No activity detected at any baud rate."));
        DEBUG_SERIAL.println(F("  Check: VCC connected? TFT_DET tied to GND? Level shifter wired?"));
    }
}


// ── Phase 2: Active protocol probe ───────────────────────────────────────────

void phase2_active_probe(uint32_t baud) {
    DEBUG_SERIAL.println(F("\n╔══════════════════════════════════════════╗"));
    DEBUG_SERIAL.println(F("║  PHASE 2 — Active protocol probe         ║"));
    DEBUG_SERIAL.println(F("╚══════════════════════════════════════════╝"));
    DEBUG_SERIAL.print(F("Using baud rate: "));
    DEBUG_SERIAL.println(baud);
    DEBUG_SERIAL.println();

    TFT_SERIAL.begin(baud);
    delay(200);
    while (TFT_SERIAL.available()) TFT_SERIAL.read();

    // ── AT commands ──────────────────────────────────────────────────────────
    DEBUG_SERIAL.println(F("--- AT Command Set ---"));
    {
        const char* at_cmds[] = {
            "AT\r\n", "AT+INFO\r\n", "AT+VER\r\n",
            "AT+RESET\r\n", "AT+BAUD?\r\n", "AT+DISP=1\r\n",
        };
        for (auto cmd : at_cmds) {
            TFT_SERIAL.print(cmd);
            uint8_t rbuf[64]; uint8_t rlen = 0;
            uint32_t t = millis();
            while (millis() - t < 300 && rlen < 63) {
                if (TFT_SERIAL.available()) rbuf[rlen++] = TFT_SERIAL.read();
            }
            DEBUG_SERIAL.print(F("  ["));
            DEBUG_SERIAL.print(cmd);
            DEBUG_SERIAL.print(F("]: "));
            if (rlen) print_hex_ascii(rbuf, rlen, 0);
            else      DEBUG_SERIAL.println(F("(no response)"));
        }
    }

    // ── DMT protocol (0xAA frame) ─────────────────────────────────────────
    DEBUG_SERIAL.println(F("\n--- DMT/DWIN Protocol Probes ---"));
    {
        const uint8_t dmt_ver[]   = {0xAA, 0x00, 0xCC, 0x33, 0xC3, 0x3C};
        const uint8_t dmt_bkl[]   = {0xAA, 0x30, 0x01, 0xFF, 0xCC, 0x33, 0xC3, 0x3C};
        const uint8_t dmt_page0[] = {0xAA, 0x03, 0x00, 0x00, 0xCC, 0x33, 0xC3, 0x3C};
        send_probe(dmt_ver,   sizeof(dmt_ver),   "DMT get-version");
        send_probe(dmt_bkl,   sizeof(dmt_bkl),   "DMT backlight ON");
        send_probe(dmt_page0, sizeof(dmt_page0),  "DMT page-0");
    }

    // ── DWIN T5UID1 protocol (0x5A 0xA5 frame) ───────────────────────────
    DEBUG_SERIAL.println(F("\n--- DWIN T5UID1 Protocol Probes ---"));
    {
        const uint8_t dwin_ver[] = {0x5A, 0xA5, 0x03, 0x81, 0x00, 0x0F};
        const uint8_t dwin_hsk[] = {0x5A, 0xA5, 0x03, 0x81, 0x00, 0x00};
        send_probe(dwin_ver, sizeof(dwin_ver), "DWIN version query");
        send_probe(dwin_hsk, sizeof(dwin_hsk), "DWIN handshake");
    }

    // ── Sync / wake bytes ─────────────────────────────────────────────────
    DEBUG_SERIAL.println(F("\n--- Sync / Wake byte probes ---"));
    {
        const uint8_t sync_ff[]   = {0xFF, 0xFF, 0xFF};
        const uint8_t sync_00[]   = {0x00, 0x00, 0x00};
        const uint8_t sync_5a[]   = {0x5A, 0xA5};
        const uint8_t sync_a5[]   = {0xA5, 0x5A};
        const uint8_t sync_eb90[] = {0xEB, 0x90, 0x00};
        const uint8_t sync_fc[]   = {0xFC, 0xDF, 0x01};
        const uint8_t nulls[16]   = {};
        send_probe(sync_ff,   sizeof(sync_ff),   "0xFF x3");
        send_probe(sync_00,   sizeof(sync_00),   "0x00 x3");
        send_probe(sync_5a,   sizeof(sync_5a),   "0x5A 0xA5");
        send_probe(sync_a5,   sizeof(sync_a5),   "0xA5 0x5A");
        send_probe(sync_eb90, sizeof(sync_eb90), "0xEB 0x90 (UN-series)");
        send_probe(sync_fc,   sizeof(sync_fc),   "0xFC 0xDF 0x01 (UN2572 candidate)");
        send_probe(nulls,     sizeof(nulls),     "16x 0x00");
    }

    TFT_SERIAL.end();
}


// ── Phase 3: Interactive hex passthrough ─────────────────────────────────────

#if INTERACTIVE_MS > 0
void phase3_interactive(uint32_t baud) {
    DEBUG_SERIAL.println(F("\n╔══════════════════════════════════════════╗"));
    DEBUG_SERIAL.println(F("║  PHASE 3 — Interactive hex passthrough   ║"));
    DEBUG_SERIAL.println(F("╚══════════════════════════════════════════╝"));
    DEBUG_SERIAL.println(F("Type hex bytes separated by spaces, press Enter to send."));
    DEBUG_SERIAL.println(F("Example: AA 30 01 FF CC 33 C3 3C"));

    TFT_SERIAL.begin(baud);
    uint32_t deadline = millis() + INTERACTIVE_MS;
    String   input_buf = "";

    while (millis() < deadline) {
        while (DEBUG_SERIAL.available()) {
            char c = DEBUG_SERIAL.read();
            if (c == '\n' || c == '\r') {
                input_buf.trim();
                if (input_buf.length() > 0) {
                    DEBUG_SERIAL.print(F("Sending: "));
                    int idx = 0;
                    while (idx + 1 < (int)input_buf.length()) {
                        while (idx < (int)input_buf.length() && input_buf[idx] == ' ') idx++;
                        if (idx + 1 >= (int)input_buf.length()) break;
                        char hi = input_buf[idx];
                        char lo = input_buf[idx + 1];
                        uint8_t b = (uint8_t)(
                            (isDigit(hi) ? hi-'0' : toupper(hi)-'A'+10) << 4 |
                            (isDigit(lo) ? lo-'0' : toupper(lo)-'A'+10)
                        );
                        TFT_SERIAL.write(b);
                        if (b < 0x10) DEBUG_SERIAL.print('0');
                        DEBUG_SERIAL.print(b, HEX);
                        DEBUG_SERIAL.print(' ');
                        idx += 2;
                    }
                    DEBUG_SERIAL.println();
                    input_buf = "";
                }
            } else {
                input_buf += c;
            }
        }

        uint8_t rbuf[64]; uint8_t rlen = 0;
        uint32_t t = millis();
        while (millis() - t < 50 && rlen < sizeof(rbuf)) {
            if (TFT_SERIAL.available()) rbuf[rlen++] = TFT_SERIAL.read();
        }
        if (rlen > 0) {
            DEBUG_SERIAL.print(F("← "));
            print_hex_ascii(rbuf, rlen, 0);
        }
    }
    TFT_SERIAL.end();
}
#endif


// ── Arduino entry points ──────────────────────────────────────────────────────

void setup() {
    DEBUG_SERIAL.begin(DEBUG_BAUD);
    while (!DEBUG_SERIAL) {}

    DEBUG_SERIAL.println(F("\n╔══════════════════════════════════════════════════╗"));
    DEBUG_SERIAL.println(F("║  XW-TS0526 / UN2572 TFT UART Probe Tool          ║"));
    DEBUG_SERIAL.println(F("║  SoftwareSerial: TX=pin2, RX=pin3 (P3 connector) ║"));
    DEBUG_SERIAL.println(F("╚══════════════════════════════════════════════════╝"));
    DEBUG_SERIAL.println(F("Wiring check:"));
    DEBUG_SERIAL.println(F("  Pin 2 (TX) → HV1 → LV1 ─┐"));
    DEBUG_SERIAL.println(F("  Pin 3 (RX) → HV2 → LV2 ─┴── U_TX.RX pad"));
    DEBUG_SERIAL.println(F("  P3 5V      → HV  (level shifter reference)"));
    DEBUG_SERIAL.println(F("  P3 3.3V    → LV  (level shifter reference)"));
    DEBUG_SERIAL.println(F("  P3 3.3V    → display VCC"));
    DEBUG_SERIAL.println(F("  P3 GND     → display GND + TFT_DET pad"));
    DEBUG_SERIAL.println();
    delay(500);

    phase1_passive_sweep();

    if (best_baud_idx >= 0) {
        phase2_active_probe(BAUD_LIST[best_baud_idx]);
    } else {
        DEBUG_SERIAL.println(F("\nNo passive hits — probing all rates blindly."));
        for (uint8_t bi = 0; bi < BAUD_COUNT; bi++)
            phase2_active_probe(BAUD_LIST[bi]);
    }

#if INTERACTIVE_MS > 0
    uint32_t use_baud = (best_baud_idx >= 0) ? BAUD_LIST[best_baud_idx] : 9600;
    phase3_interactive(use_baud);
#endif

    DEBUG_SERIAL.println(F("\n══ Probe complete. Reset to run again. ══"));
    DEBUG_SERIAL.println(F("Next steps:"));
    DEBUG_SERIAL.println(F("  1. Find the baud rate with the most bytes / readable ASCII."));
    DEBUG_SERIAL.println(F("  2. ACK responses: 'OK' = AT protocol, 0xAA reply = DMT, 0x5A 0xA5 = DWIN."));
    DEBUG_SERIAL.println(F("  3. Set INTERACTIVE_MS = 60000 and reflash to probe manually."));
}

void loop() {}
