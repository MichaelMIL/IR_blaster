// Custom firmware for a Tuya IR blaster (CB3S / BK7231N) built on LibreTiny.
//
// Serial console on UART1 (TX1/RX1 pads), 115200 baud. Type `help`.
// WiFi credentials and pin assignments are stored in flash and survive reboots.

#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "ir_nec.h"
#include "app.h"
#include "recordings.h"

#ifndef IR_CARRIER_HZ
#define IR_CARRIER_HZ 38000
#endif

static IrReceiver rx;
static IrSender   tx;
static String     line;
static bool       wifiWasConnected = false;
static uint32_t   wifiStartedAt = 0;

// Last raw capture (for `save` / replay of non-NEC remotes such as AC units)
static uint32_t lastCapture[IrReceiver::MAX_EDGES];
static int      lastN = 0;
static uint32_t playBuf[REC_MAX_PULSES];
int appLastPulses() { return lastN; }
const uint32_t *appLastCapture() { return lastCapture; }

// Status LED
static uint32_t statusFlashUntil = 0;
static void statusWrite(bool on) {
    if (cfg.statusPin == NO_PIN) return;
    digitalWrite(cfg.statusPin, (on != cfg.statusInvert) ? HIGH : LOW);
}
void appApplyStatusLed() {
    if (cfg.statusPin == NO_PIN) return;
    pinMode(cfg.statusPin, OUTPUT);
    statusWrite(false);
}
void appStatusBlink(uint32_t ms) { statusFlashUntil = millis() + ms; }
static void statusLedTick() {
    if (cfg.statusPin == NO_PIN) return;
    bool base;
    if (WiFi.status() == WL_CONNECTED) base = true;                 // steady on
    else if (cfg.hasWifi())            base = (millis() / 500) & 1; // 1 Hz blink while connecting
    else                               base = false;                // off
    bool flash = (int32_t)(millis() - statusFlashUntil) < 0;
    statusWrite(flash ? !base : base);
}

// Ring buffer of received frames (shared with the web UI)
static RxRecord rxHist[RX_HISTORY];
static int      rxHead = 0, rxLen = 0;
static uint32_t rxSeq = 0;

static void recordFrame(const IrFrame &f) {
    RxRecord &r = rxHist[(rxHead + rxLen) % RX_HISTORY];
    if (rxLen < RX_HISTORY) rxLen++; else rxHead = (rxHead + 1) % RX_HISTORY;
    r.t = millis();
    r.type = f.decoded ? (f.repeat ? 2 : 1) : 0;
    r.address = f.address; r.command = f.command; r.raw = f.raw; r.pulses = f.pulses;
    rxSeq++;
}

int appGetReceived(RxRecord *out, int max) {
    int n = rxLen < max ? rxLen : max;
    for (int i = 0; i < n; i++) out[i] = rxHist[(rxHead + (rxLen - n) + i) % RX_HISTORY];
    return n;
}
uint32_t appReceivedSeq() { return rxSeq; }
void appClearReceived() { rxHead = rxLen = 0; rxSeq++; }

// ------------------------------------------------------------------ helpers

static String ipStr(IPAddress ip) {
    return String(ip[0]) + "." + ip[1] + "." + ip[2] + "." + ip[3];
}

static const char *pinName(uint8_t pin) {
    static char buf[8];
    if (pin == NO_PIN) return "none";
    snprintf(buf, sizeof(buf), "P%u", pin);
    return buf;
}

// Accepts "26", "P26", "p26", "none"
static bool parsePin(const String &s, uint8_t &out) {
    if (s == "none" || s == "-") { out = NO_PIN; return true; }
    const char *p = s.c_str();
    if (*p == 'P' || *p == 'p') p++;
    if (!*p) return false;
    char *end;
    long v = strtol(p, &end, 10);
    if (*end || v < 0 || v > 26) return false;
    out = (uint8_t)v;
    return true;
}

static void applyTxPins();
static void applyRxPin();
void appApplyPins() { applyTxPins(); applyRxPin(); }
int  appTxPinCount() { return tx.pinCount(); }
static void applyRxPin();
bool appSendNec(uint8_t a, uint8_t c) { if (!tx.pinCount()) return false; rx.end(); tx.sendNec(a, c); applyRxPin(); appStatusBlink(120); return true; }
bool appSendRaw(uint32_t w) { if (!tx.pinCount()) return false; rx.end(); tx.sendNecRaw(w); applyRxPin(); appStatusBlink(120); return true; }
bool appWifiConnected() { return WiFi.status() == WL_CONNECTED; }

static void applyTxPins() {
    uint8_t pins[LED_COUNT];
    int n = cfg.activeLedPins(pins);
    tx.setPins(pins, n);
}

static void applyRxPin() {
    if (cfg.rxPin == NO_PIN) rx.end();
    else rx.begin(cfg.rxPin);
}

static void printFrame(const IrFrame &f) {
    if (f.decoded && f.repeat) {
        Serial.println("RX NEC repeat");
    } else if (f.decoded) {
        Serial.printf("RX NEC addr=0x%02X cmd=0x%02X raw=0x%08lX%s\n",
                      f.address, f.command, (unsigned long)f.raw,
                      f.extended ? " (extended)" : "");
    } else {
        Serial.printf("RX raw %d pulses (not NEC) - 'save <name>' keeps it\n", f.pulses);
    }
}

static void printPulses(const uint32_t *p, int n) {
    Serial.print("   timings(us):");
    for (int i = 0; i < n && i < 40; i++) Serial.printf(" %lu", (unsigned long)p[i]);
    if (n > 40) Serial.printf(" ... (+%d)", n - 40);
    Serial.println();
}

void appWifiBegin();
static void wifiBegin() { appWifiBegin(); }
String appIp() { return appWifiConnected() ? ipStr(WiFi.localIP()) : String("-"); }

void appWifiBegin() {
    if (!cfg.hasWifi()) {
        Serial.println("wifi: no credentials set (use: wifi ssid <name> / wifi pass <pw>)");
        return;
    }
    Serial.printf("wifi: connecting to '%s'...\n", cfg.ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("ir-blaster");
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
    wifiStartedAt = millis();
    wifiWasConnected = false;
}

static void wifiStatus() {
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("wifi: connected to '%s' ip=%s rssi=%d dBm  web: http://%s/ (http://ir-blaster.local/)\n",
                      cfg.ssid.c_str(), ipStr(WiFi.localIP()).c_str(), WiFi.RSSI(), ipStr(WiFi.localIP()).c_str());
    } else if (cfg.hasWifi()) {
        Serial.printf("wifi: not connected (status=%d), ssid='%s'\n", (int)WiFi.status(), cfg.ssid.c_str());
    } else {
        Serial.println("wifi: no credentials set");
    }
}

static void printStatus() {
    Serial.printf("firmware: ir_blaster on %s, LibreTiny %s, uptime %lus\n",
                  LT_BOARD_STR, lt_get_version(), (unsigned long)(millis() / 1000));
    wifiStatus();
    Serial.print("pins:");
    for (int i = 0; i < LED_COUNT; i++) Serial.printf(" led%d=%s", i + 1, pinName(cfg.ledPin[i]));
    Serial.printf(" rx=%s status=%s%s carrier=%dHz\n", pinName(cfg.rxPin), pinName(cfg.statusPin),
                  cfg.statusInvert ? "(inv)" : "", IR_CARRIER_HZ);
}

static void printHelp() {
    Serial.println(
        "commands:\n"
        "  status                       firmware / wifi / pin summary\n"
        "  wifi ssid <name>             set network name (spaces allowed)\n"
        "  wifi pass <password>         set password\n"
        "  wifi connect | status | clear\n"
        "  send <addr> <cmd>            send NEC via all configured LEDs\n"
        "  raw <hex32>                  send raw 32-bit NEC word\n"
        "recordings (any remote, e.g. AC):\n"
        "  rec <name> [sec]             wait for the next key press and store it\n"
        "  save <name>                  store the last received frame\n"
        "  play <name>                  retransmit a recording\n"
        "  list | del <name> | dump <name>\n"
        "  export                       JSON of last capture + all recordings (also GET /api/export)\n"
        "  txtest <name>                play with receiver on, compare what came back (hold a hand in front)\n"
        "  txcal | txmode pwm|bitbang   carrier diagnostics\n"
        "test mode:\n"
        "  led <1-3> pin <P#|none>      assign GPIO to an IR LED\n"
        "  led <1-3> test [sec]         blink carrier on that LED only (watch via phone camera)\n"
        "  led all test [sec]           blink all configured LEDs\n"
        "  led scan [sec]               blink every candidate GPIO in turn to find the LEDs\n"
        "  rx pin <P#|none>             assign GPIO to the IR receiver\n"
        "  rx test [sec]                live readings: activity per second + decoded frames\n"
        "  rx scan [sec]                watch every candidate GPIO, report which one sees IR\n"
        "  sled pin <P#|none> | sled invert on|off | sled test   (blue status LED)\n"
        "  reset                        factory defaults (pins + wifi)\n"
        "  reboot");
}

// --------------------------------------------------------------- test mode

static void ledTest(int idx, uint32_t seconds);
void appLedTest(int idx, uint32_t seconds) { ledTest(idx, seconds); }
static void ledTest(int idx, uint32_t seconds) {   // idx -1 = all
    uint8_t pins[LED_COUNT];
    int n = 0;
    if (idx < 0) {
        n = cfg.activeLedPins(pins);
    } else if (cfg.ledPin[idx] != NO_PIN) {
        pins[n++] = cfg.ledPin[idx];
    }
    if (n == 0) { Serial.println("no pin assigned"); return; }

    Serial.print("blinking");
    for (int i = 0; i < n; i++) Serial.printf(" %s", pinName(pins[i]));
    Serial.printf(" for %lus (200ms on / 200ms off @ %dHz)\n", (unsigned long)seconds, IR_CARRIER_HZ);
    tx.setPins(pins, n);
    tx.burst(200, 200, seconds * 1000);
    applyTxPins();
    Serial.println("done");
}

void appBlinkPin(uint8_t pin, uint32_t seconds) {
    bool rxWasHere = (cfg.rxPin == pin);
    if (rxWasHere) rx.end();
    tx.setPins(&pin, 1);
    tx.burst(200, 200, seconds * 1000);
    applyTxPins();
    if (rxWasHere) applyRxPin();
    appApplyStatusLed();
}

static void ledScan(uint32_t secPerPin) {
    Serial.printf("led scan: each candidate GPIO blinks for %lus. Watch the LEDs through a phone camera.\n",
                  (unsigned long)secPerPin);
    Serial.println("(UART1 P10/P11 skipped; receiver detached during scan)");
    rx.end();
    for (int i = 0; i < CANDIDATE_COUNT; i++) {
        uint8_t p = CANDIDATE_PINS[i];
        Serial.printf(">>> %s\n", pinName(p));
        tx.setPins(&p, 1);
        tx.burst(200, 200, secPerPin * 1000);
        while (Serial.available()) {                  // any key aborts
            if (Serial.read() == 'q') { i = CANDIDATE_COUNT; break; }
        }
    }
    applyTxPins();
    applyRxPin();
    appApplyStatusLed();
    Serial.println("led scan done. Assign with: led <n> pin P#");
}

static void rxTest(uint32_t seconds) {
    if (!rx.active()) { Serial.println("no rx pin assigned"); return; }
    Serial.printf("rx test on %s for %lus: press remote buttons. ('q' to stop)\n",
                  pinName(cfg.rxPin), (unsigned long)seconds);
    Serial.printf("idle level: %s (receivers idle HIGH; LOW means wrong pin or no power)\n",
                  digitalRead(cfg.rxPin) ? "HIGH" : "LOW");
    uint32_t end = millis() + seconds * 1000;
    uint32_t nextTick = millis() + 1000;
    uint32_t lastEdges = rx.edgeCount();
    uint32_t lastNoise = rx.noiseCount();
    uint32_t pulses[IrReceiver::MAX_EDGES];
    while ((int32_t)(millis() - end) < 0) {
        IrFrame f;
        if (rx.poll(f, lastCapture, IrReceiver::MAX_EDGES)) { lastN = f.pulses; printFrame(f); recordFrame(f); }
        // Peek raw timings while a frame is still buffered: copy on next poll instead
        if ((int32_t)(millis() - nextTick) >= 0) {
            nextTick += 1000;
            uint32_t e = rx.edgeCount();
            uint32_t nz = rx.noiseCount();
            Serial.printf("   activity: %lu edges/s%s\n", (unsigned long)(e - lastEdges),
                          nz != lastNoise ? " (glitches ignored)" : "");
            lastEdges = e; lastNoise = nz;
        }
        if (Serial.available() && Serial.read() == 'q') break;
        delay(1);
    }
    (void)pulses;
    Serial.println("rx test done");
}

void appRxScan(uint32_t seconds, ScanResult &r) {
    rx.end();
    uint8_t ledPins[LED_COUNT];
    int nLed = cfg.activeLedPins(ledPins);
    tx.setPins(nullptr, 0);                        // release LED pins so they can be sampled too
    int last[CANDIDATE_COUNT];
    for (int i = 0; i < CANDIDATE_COUNT; i++) {
        r.transitions[i] = 0;
        pinMode(CANDIDATE_PINS[i], INPUT_PULLUP);
        last[i] = digitalRead(CANDIDATE_PINS[i]);
    }
    uint32_t end = millis() + seconds * 1000;
    while ((int32_t)(millis() - end) < 0) {
        for (int i = 0; i < CANDIDATE_COUNT; i++) {
            int v = digitalRead(CANDIDATE_PINS[i]);
            if (v != last[i]) { r.transitions[i]++; last[i] = v; }
        }
    }
    r.best = -1;
    for (int i = 0; i < CANDIDATE_COUNT; i++) {
        r.idle[i] = last[i];
        if (r.transitions[i] > 20 && (r.best < 0 || r.transitions[i] > r.transitions[r.best])) r.best = i;
    }
    tx.setPins(ledPins, nLed);
    applyRxPin();
    appApplyStatusLed();
}

static void rxScan(uint32_t seconds) {
    Serial.printf("rx scan: watching %d GPIOs for %lus. Point a remote at the receiver and press buttons.\n",
                  CANDIDATE_COUNT, (unsigned long)seconds);
    ScanResult r;
    appRxScan(seconds, r);
    Serial.println("pin   transitions  idle");
    for (int i = 0; i < CANDIDATE_COUNT; i++)
        Serial.printf("%-5s %-12lu %s\n", pinName(CANDIDATE_PINS[i]),
                      (unsigned long)r.transitions[i], r.idle[i] ? "HIGH" : "LOW");
    if (r.best >= 0)
        Serial.printf("=> likely receiver pin: %s   (set with: rx pin %s)\n",
                      pinName(CANDIDATE_PINS[r.best]), pinName(CANDIDATE_PINS[r.best]));
    else
        Serial.println("=> no IR activity seen on any pin");
}

// --------------------------------------------------------------- recordings

static bool validName(const char *n) {
    size_t l = strlen(n);
    if (l == 0 || l > REC_NAME_LEN) return false;
    for (size_t i = 0; i < l; i++) if (n[i] == ' ' || n[i] == '"' || n[i] == '\\') return false;
    return true;
}

bool appSaveLast(const char *name, String &msg) {
    if (!validName(name)) { msg = "name: 1-23 chars, no spaces"; return false; }
    if (lastN < 3) { msg = "nothing captured yet"; return false; }
    int before = lastN;
    lastN = irCleanGlitches(lastCapture, lastN);
    if (lastN != before) Serial.printf("cleaned %d receiver glitch(es) before saving\n", (before - lastN) / 2);
    if (!recSave(name, lastCapture, lastN)) { msg = "save failed (32 slots full?)"; return false; }
    msg = "saved '" + String(name) + "' (" + String(lastN) + " pulses)";
    return true;
}

int appRecord(const char *name, uint32_t waitSec, String &msg) {
    if (!validName(name)) { msg = "name: 1-23 chars, no spaces"; return -1; }
    if (!rx.active()) { msg = "no receiver pin assigned (run rx scan)"; return -1; }
    uint32_t end = millis() + waitSec * 1000;
    IrFrame f;
    while ((int32_t)(millis() - end) < 0) {
        if (rx.poll(f, lastCapture, IrReceiver::MAX_EDGES)) {
            lastN = f.pulses < IrReceiver::MAX_EDGES ? f.pulses : IrReceiver::MAX_EDGES;
            if (lastN < 3) continue;                       // noise
            printFrame(f); recordFrame(f);
            return appSaveLast(name, msg) ? lastN : -1;
        }
        statusLedTick();
        delay(1);
    }
    msg = "timeout: no IR received in " + String(waitSec) + " s";
    return 0;
}

bool appPlay(const char *name, String &msg) {
    int slot = recFind(name);
    if (slot < 0) { msg = "no recording named '" + String(name) + "'"; return false; }
    int n = recLoad(slot, playBuf, REC_MAX_PULSES);
    if (n <= 0) { msg = "recording unreadable"; return false; }
    if (tx.pinCount() == 0) { msg = "no LED pins assigned"; return false; }
    n = irCleanGlitches(playBuf, n);            // older recordings may still contain dropouts
    rx.end();
    tx.sendTimings(playBuf, n);
    applyRxPin();
    appStatusBlink(150);
    msg = "played '" + String(name) + "' (" + String(n) + " pulses)";
    return true;
}

// Loopback: play a recording while the receiver keeps listening, then compare.
// Works when some LED light reflects back into the receiver (hold a hand or
// paper ~10 cm in front of the device).
static void txTest(const char *name) {
    int slot = recFind(name);
    if (slot < 0) { Serial.println("not found"); return; }
    int n = recLoad(slot, playBuf, REC_MAX_PULSES);
    n = irCleanGlitches(playBuf, n);
    if (!rx.active()) { Serial.println("no rx pin"); return; }
    if (tx.pinCount() == 0) { Serial.println("no LED pins"); return; }
    Serial.printf("txtest '%s': sending %d pulses (%s carrier), receiver stays on\n", name, n, tx.pwm() ? "PWM" : "bit-bang");
    IrFrame f; while (rx.poll(f)) {}                     // flush
    tx.sendTimings(playBuf, n);
    uint32_t end = millis() + 300; bool got = false;
    while ((int32_t)(millis() - end) < 0) {
        if (rx.poll(f, lastCapture, IrReceiver::MAX_EDGES)) {
            lastN = f.pulses; got = true;
            Serial.printf("  received back: %d pulses (sent %d)\n", lastN, n);
            int k = lastN < n ? lastN : n; int bad = 0; long sumErr = 0;
            for (int i = 0; i < k; i++) {
                long e = (long)lastCapture[i] - (long)playBuf[i];
                sumErr += e < 0 ? -e : e;
                if ((e < 0 ? -e : e) > (long)(playBuf[i] / 4) + 60) bad++;
            }
            Serial.printf("  mean |error| %ld us over %d pulses, %d pulse(s) off by >25%%\n", k ? sumErr / k : 0, k, bad);
            Serial.print("  sent:     "); for (int i = 0; i < 12 && i < n; i++) Serial.printf("%lu ", (unsigned long)playBuf[i]); Serial.println();
            Serial.print("  received: "); for (int i = 0; i < 12 && i < lastN; i++) Serial.printf("%lu ", (unsigned long)lastCapture[i]); Serial.println();
            break;
        }
        delay(1);
    }
    if (!got) Serial.println("  nothing received back: no reflection reaching the receiver, or the LED is not on this pin");
}

static void listRecordings() {
    RecInfo info[REC_SLOTS];
    int n = recList(info, REC_SLOTS);
    if (n == 0) { Serial.println("no recordings (use: rec <name>)"); return; }
    Serial.println("slot  name                     pulses  ms");
    for (int i = 0; i < n; i++)
        Serial.printf("%-5d %-24s %-7u %lu\n", info[i].slot, info[i].name, info[i].pulses, (unsigned long)info[i].durationMs);
}

// JSON export of everything (same format as GET /api/export)
static void printTimingsJson(const uint32_t *t, int n) {
    Serial.print("[");
    for (int i = 0; i < n; i++) { Serial.print(t[i]); if (i + 1 < n) Serial.print(","); }
    Serial.print("]");
}
static void exportJson() {
    Serial.printf("{\"device\":\"ir-blaster\",\"carrier_hz\":%d,\"unit\":\"us\",\"format\":\"mark,space,mark,...\",", IR_CARRIER_HZ);
    Serial.printf("\"last_capture\":{\"pulses\":%d,\"timings\":", lastN); printTimingsJson(lastCapture, lastN);
    Serial.print("},\"recordings\":[");
    RecInfo info[REC_SLOTS];
    int n = recList(info, REC_SLOTS);
    for (int i = 0; i < n; i++) {
        int k = recLoad(info[i].slot, playBuf, REC_MAX_PULSES);
        Serial.printf("{\"name\":\"%s\",\"pulses\":%d,\"timings\":", info[i].name, k); printTimingsJson(playBuf, k);
        Serial.print(i + 1 < n ? "}," : "}");
    }
    Serial.println("]}");
}

static void dumpRecording(const char *name) {
    int slot = recFind(name);
    if (slot < 0) { Serial.println("not found"); return; }
    int n = recLoad(slot, playBuf, REC_MAX_PULSES);
    Serial.printf("%s: %d pulses (us, mark first):\n", name, n);
    for (int i = 0; i < n; i++) Serial.printf("%lu%s", (unsigned long)playBuf[i], (i + 1 < n) ? "," : "\n");
}

// ---------------------------------------------------------------- commands

static int split(const String &s, String *tok, int max) {
    int n = 0, i = 0, len = s.length();
    while (i < len && n < max) {
        while (i < len && s[i] == ' ') i++;
        int st = i;
        while (i < len && s[i] != ' ') i++;
        if (i > st) tok[n++] = s.substring(st, i);
    }
    return n;
}

static uint32_t argSeconds(const String *t, int n, int idx, uint32_t def) {
    if (idx < n) { long v = t[idx].toInt(); if (v > 0) return (uint32_t)v; }
    return def;
}

static void handleCommand(const String &cmd) {
    String t[6];
    int n = split(cmd, t, 6);
    if (n == 0) return;
    const String &c = t[0];

    if (c == "help") { printHelp(); return; }
    if (c == "status") { printStatus(); return; }
    if (c == "reboot") { Serial.println("rebooting"); delay(100); lt_reboot(); return; }
    if (c == "reset") {
        cfg.reset(); cfg.save(); applyTxPins(); applyRxPin(); appApplyStatusLed();
        Serial.println("config reset to defaults"); printStatus(); return;
    }

    if (c == "wifi") {
        if (n >= 3 && t[1] == "ssid") {
            cfg.ssid = cmd.substring(cmd.indexOf("ssid") + 5); cfg.ssid.trim();
            cfg.save(); Serial.printf("wifi ssid='%s' saved\n", cfg.ssid.c_str()); return;
        }
        if (n >= 3 && t[1] == "pass") {
            cfg.pass = cmd.substring(cmd.indexOf("pass") + 5); cfg.pass.trim();
            cfg.save(); Serial.println("wifi password saved"); return;
        }
        if (n >= 2 && t[1] == "connect") { wifiBegin(); return; }
        if (n >= 2 && t[1] == "status")  { wifiStatus(); return; }
        if (n >= 2 && t[1] == "clear") {
            cfg.ssid = ""; cfg.pass = ""; cfg.save(); WiFi.disconnect(true);
            Serial.println("wifi credentials cleared"); return;
        }
        Serial.println("usage: wifi ssid <name> | wifi pass <pw> | wifi connect | wifi status | wifi clear");
        return;
    }

    if (c == "rec" || c == "record") {
        if (n < 2) { Serial.println("usage: rec <name> [sec]   - waits for the next remote key press and stores it"); return; }
        Serial.printf("recording '%s': press the key on the remote now (%lus)...\n", t[1].c_str(), (unsigned long)argSeconds(t, n, 2, 15));
        String msg; appRecord(t[1].c_str(), argSeconds(t, n, 2, 15), msg); Serial.println(msg); return;
    }
    if (c == "save") {
        if (n < 2) { Serial.println("usage: save <name>   - stores the last received frame"); return; }
        String msg; appSaveLast(t[1].c_str(), msg); Serial.println(msg); return;
    }
    if (c == "play") {
        if (n < 2) { Serial.println("usage: play <name>"); return; }
        String msg; appPlay(t[1].c_str(), msg); Serial.println(msg); return;
    }
    if (c == "list") { listRecordings(); return; }
    if (c == "txtest") { if (n < 2) Serial.println("usage: txtest <name>"); else txTest(t[1].c_str()); return; }
    if (c == "txcal") {
        Serial.printf("tx mode: %s; bit-bang carrier would be %lu Hz (target %d)\n",
                      tx.pwm() ? "hardware PWM (38.46 kHz)" : "bit-bang", (unsigned long)tx.measureBitbangHz(), IR_CARRIER_HZ);
        return;
    }
    if (c == "txmode") {
        if (n >= 2 && (t[1] == "pwm" || t[1] == "bitbang")) { tx.setPwm(t[1] == "pwm"); Serial.printf("tx mode: %s\n", t[1].c_str()); }
        else Serial.println("usage: txmode pwm|bitbang   (not saved; default pwm)");
        return;
    }
    if (c == "export") { exportJson(); return; }
    if (c == "fake") {                       // synthetic NEC 0x04/0x08 capture, for testing the storage path
        uint32_t w = 0x04 | (0xFB << 8) | (0x08 << 16) | (0xF7 << 24);
        int k = 0; lastCapture[k++] = 9000; lastCapture[k++] = 4500;
        for (int i = 0; i < 32; i++) { lastCapture[k++] = 562; lastCapture[k++] = (w >> i) & 1 ? 1687 : 562; }
        lastCapture[k++] = 562; lastN = k;
        Serial.printf("fake capture loaded (%d pulses)\n", lastN); return;
    }
    if (c == "del" || c == "delete") {
        if (n < 2) { Serial.println("usage: del <name>"); return; }
        Serial.println(recErase(t[1].c_str()) ? "deleted" : "not found"); return;
    }
    if (c == "dump") {
        if (n < 2) { Serial.println("usage: dump <name>"); return; }
        dumpRecording(t[1].c_str()); return;
    }
    if (c == "sled") {
        if (n >= 3 && t[1] == "pin") {
            uint8_t p;
            if (!parsePin(t[2], p)) { Serial.println("bad pin"); return; }
            if (p == 10 || p == 11) { Serial.println("P10/P11 are the serial console, refusing"); return; }
            if (cfg.statusPin != NO_PIN) pinMode(cfg.statusPin, INPUT);
            cfg.statusPin = p; cfg.save(); appApplyStatusLed();
            Serial.printf("status led pin=%s saved\n", pinName(p)); return;
        }
        if (n >= 3 && t[1] == "invert") {
            cfg.statusInvert = (t[2] == "on" || t[2] == "1" || t[2] == "true"); cfg.save();
            Serial.printf("status led invert=%s saved\n", cfg.statusInvert ? "on" : "off"); return;
        }
        if (n >= 2 && t[1] == "test") {
            Serial.println("status led: 5 blinks");
            for (int i = 0; i < 5; i++) { statusWrite(true); delay(200); statusWrite(false); delay(200); }
            return;
        }
        Serial.println("usage: sled pin <P#|none> | sled invert on|off | sled test"); return;
    }

    if (c == "send") {
        unsigned addr = 0, command = 0;
        if (n >= 3 && sscanf(t[1].c_str(), "%i", &addr) == 1 && sscanf(t[2].c_str(), "%i", &command) == 1) {
            if (tx.pinCount() == 0) { Serial.println("no LED pins assigned"); return; }
            Serial.printf("TX NEC addr=0x%02X cmd=0x%02X on %d LED pin(s)\n", addr, command, tx.pinCount());
            tx.sendNec(addr, command);
        } else Serial.println("usage: send <addr> <cmd>   e.g. send 0x04 0x08");
        return;
    }
    if (c == "raw") {
        if (n >= 2) {
            unsigned long word = strtoul(t[1].c_str(), nullptr, 0);
            if (tx.pinCount() == 0) { Serial.println("no LED pins assigned"); return; }
            Serial.printf("TX NEC raw=0x%08lX on %d LED pin(s)\n", word, tx.pinCount());
            tx.sendNecRaw(word);
        } else Serial.println("usage: raw <hex32>   e.g. raw 0x20DF10EF");
        return;
    }

    if (c == "led") {
        if (n >= 2 && t[1] == "scan") { ledScan(argSeconds(t, n, 2, 2)); return; }
        if (n >= 3 && t[1] == "all" && t[2] == "test") { ledTest(-1, argSeconds(t, n, 3, 3)); return; }
        int idx = t[1].toInt() - 1;
        if (n >= 3 && idx >= 0 && idx < LED_COUNT) {
            if (t[2] == "pin" && n >= 4) {
                uint8_t p;
                if (!parsePin(t[3], p)) { Serial.println("bad pin (use P6..P26 or none)"); return; }
                if (p == 10 || p == 11) { Serial.println("P10/P11 are the serial console, refusing"); return; }
                cfg.ledPin[idx] = p; cfg.save(); applyTxPins();
                Serial.printf("led%d pin=%s saved\n", idx + 1, pinName(p)); return;
            }
            if (t[2] == "test") { ledTest(idx, argSeconds(t, n, 3, 3)); return; }
        }
        Serial.println("usage: led <1-3> pin <P#|none> | led <1-3> test [sec] | led all test [sec] | led scan [sec]");
        return;
    }

    if (c == "rx") {
        if (n >= 3 && t[1] == "pin") {
            uint8_t p;
            if (!parsePin(t[2], p)) { Serial.println("bad pin (use P6..P26 or none)"); return; }
            if (p == 10 || p == 11) { Serial.println("P10/P11 are the serial console, refusing"); return; }
            cfg.rxPin = p; cfg.save(); applyRxPin();
            Serial.printf("rx pin=%s saved\n", pinName(p)); return;
        }
        if (n >= 2 && t[1] == "test") { rxTest(argSeconds(t, n, 2, 15)); return; }
        if (n >= 2 && t[1] == "scan") { rxScan(argSeconds(t, n, 2, 10)); return; }
        Serial.println("usage: rx pin <P#|none> | rx test [sec] | rx scan [sec]");
        return;
    }

    Serial.printf("unknown command: %s (try 'help')\n", c.c_str());
}

// --------------------------------------------------------------------- main

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("=== IR blaster (CB3S / BK7231N) ===");
    cfg.load();
    appApplyStatusLed();
    for (int i = 0; i < 3; i++) { statusWrite(true); delay(100); statusWrite(false); delay(100); }
    tx.setCarrier(IR_CARRIER_HZ);
    applyTxPins();
    applyRxPin();
    printStatus();
    if (cfg.hasWifi()) wifiBegin();
    Serial.println("type 'help' for commands");
}

void loop() {
    IrFrame f;
    if (rx.poll(f, lastCapture, IrReceiver::MAX_EDGES)) {
        lastN = f.pulses < IrReceiver::MAX_EDGES ? f.pulses : IrReceiver::MAX_EDGES;
        printFrame(f); recordFrame(f); appStatusBlink(80);
    }
    statusLedTick();
    webLoop();

    if (cfg.hasWifi()) {
        bool connected = WiFi.status() == WL_CONNECTED;
        if (connected && !wifiWasConnected) {
            Serial.printf("wifi: connected, ip=%s\n", ipStr(WiFi.localIP()).c_str());
            webBegin();
        } else if (!connected && wifiWasConnected) {
            Serial.println("wifi: connection lost, reconnecting");
            WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
        }
        wifiWasConnected = connected;
    }

    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\n' || ch == '\r') {
            line.trim();
            if (line.length()) handleCommand(line);
            line = "";
        } else if (line.length() < 120) {
            line += ch;
        }
    }
}
