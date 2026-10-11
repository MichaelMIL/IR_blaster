// Custom firmware for a Tuya IR blaster (CB3S / BK7231N) built on LibreTiny.
//
// Serial console on UART1 (TX1/RX1 pads), 115200 baud. Type `help`.
// WiFi credentials and pin assignments are stored in flash and survive reboots.

#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "ir_nec.h"

#ifndef IR_CARRIER_HZ
#define IR_CARRIER_HZ 38000
#endif

static IrReceiver rx;
static IrSender   tx;
static String     line;
static bool       wifiWasConnected = false;
static uint32_t   wifiStartedAt = 0;

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
        Serial.printf("RX raw %d pulses (not NEC)\n", f.pulses);
    }
}

static void printPulses(const uint32_t *p, int n) {
    Serial.print("   timings(us):");
    for (int i = 0; i < n && i < 40; i++) Serial.printf(" %lu", (unsigned long)p[i]);
    if (n > 40) Serial.printf(" ... (+%d)", n - 40);
    Serial.println();
}

static void wifiBegin() {
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
        Serial.printf("wifi: connected to '%s' ip=%s rssi=%d dBm host=ir-blaster\n",
                      cfg.ssid.c_str(), ipStr(WiFi.localIP()).c_str(), WiFi.RSSI());
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
    Serial.printf(" rx=%s carrier=%dHz\n", pinName(cfg.rxPin), IR_CARRIER_HZ);
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
        "test mode:\n"
        "  led <1-3> pin <P#|none>      assign GPIO to an IR LED\n"
        "  led <1-3> test [sec]         blink carrier on that LED only (watch via phone camera)\n"
        "  led all test [sec]           blink all configured LEDs\n"
        "  led scan [sec]               blink every candidate GPIO in turn to find the LEDs\n"
        "  rx pin <P#|none>             assign GPIO to the IR receiver\n"
        "  rx test [sec]                live readings: activity per second + decoded frames\n"
        "  rx scan [sec]                watch every candidate GPIO, report which one sees IR\n"
        "  reset                        factory defaults (pins + wifi)\n"
        "  reboot");
}

// --------------------------------------------------------------- test mode

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
    uint32_t pulses[IrReceiver::MAX_EDGES];
    while ((int32_t)(millis() - end) < 0) {
        IrFrame f;
        if (rx.poll(f)) {
            printFrame(f);
        }
        // Peek raw timings while a frame is still buffered: copy on next poll instead
        if ((int32_t)(millis() - nextTick) >= 0) {
            nextTick += 1000;
            uint32_t e = rx.edgeCount();
            Serial.printf("   activity: %lu edges/s\n", (unsigned long)(e - lastEdges));
            lastEdges = e;
        }
        if (Serial.available() && Serial.read() == 'q') break;
        delay(1);
    }
    (void)pulses;
    Serial.println("rx test done");
}

static void rxScan(uint32_t seconds) {
    Serial.printf("rx scan: watching %d GPIOs for %lus. Point a remote at the receiver and press buttons.\n",
                  CANDIDATE_COUNT, (unsigned long)seconds);
    rx.end();
    uint8_t ledPins[LED_COUNT];
    int nLed = cfg.activeLedPins(ledPins);
    tx.setPins(nullptr, 0);                        // release LED pins so they can be sampled too
    uint32_t transitions[CANDIDATE_COUNT] = {0};
    int last[CANDIDATE_COUNT];
    for (int i = 0; i < CANDIDATE_COUNT; i++) {
        pinMode(CANDIDATE_PINS[i], INPUT_PULLUP);
        last[i] = digitalRead(CANDIDATE_PINS[i]);
    }
    uint32_t end = millis() + seconds * 1000;
    while ((int32_t)(millis() - end) < 0) {
        for (int i = 0; i < CANDIDATE_COUNT; i++) {
            int v = digitalRead(CANDIDATE_PINS[i]);
            if (v != last[i]) { transitions[i]++; last[i] = v; }
        }
    }
    Serial.println("pin   transitions  idle");
    int best = -1;
    for (int i = 0; i < CANDIDATE_COUNT; i++) {
        Serial.printf("%-5s %-12lu %s\n", pinName(CANDIDATE_PINS[i]),
                      (unsigned long)transitions[i], last[i] ? "HIGH" : "LOW");
        if (transitions[i] > 20 && (best < 0 || transitions[i] > transitions[best])) best = i;
    }
    if (best >= 0)
        Serial.printf("=> likely receiver pin: %s   (set with: rx pin %s)\n",
                      pinName(CANDIDATE_PINS[best]), pinName(CANDIDATE_PINS[best]));
    else
        Serial.println("=> no IR activity seen on any pin");
    tx.setPins(ledPins, nLed);
    applyRxPin();
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
        cfg.reset(); cfg.save(); applyTxPins(); applyRxPin();
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
    tx.setCarrier(IR_CARRIER_HZ);
    applyTxPins();
    applyRxPin();
    printStatus();
    if (cfg.hasWifi()) wifiBegin();
    Serial.println("type 'help' for commands");
}

void loop() {
    IrFrame f;
    if (rx.poll(f)) printFrame(f);

    if (cfg.hasWifi()) {
        bool connected = WiFi.status() == WL_CONNECTED;
        if (connected && !wifiWasConnected) {
            Serial.printf("wifi: connected, ip=%s\n", ipStr(WiFi.localIP()).c_str());
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
