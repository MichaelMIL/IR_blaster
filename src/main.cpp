// Minimal custom firmware for a Tuya IR blaster (CB3S / BK7231N).
//
// - Receives IR on IR_RX_PIN, decodes NEC frames and prints them on the serial console.
// - Sends NEC frames on IR_TX_PIN when you type a command on the serial console:
//       send <address> <command>      e.g.  send 0x04 0x08
//       raw  <hex32>                  e.g.  raw 0x20DF10EF   (raw 32-bit NEC word)
//
// The 38 kHz carrier is generated in software, so it works on any GPIO.
// The three IR LEDs on these boards are driven in parallel from one transistor,
// so a single TX pin lights all of them.

#include <Arduino.h>
#include "ir_nec.h"

static IrNecReceiver rx(IR_RX_PIN);
static IrNecSender   tx(IR_TX_PIN, IR_CARRIER_HZ);

static String line;

static void handleCommand(const String &cmd) {
    if (cmd.startsWith("send ")) {
        unsigned addr = 0, command = 0;
        if (sscanf(cmd.c_str() + 5, "%i %i", &addr, &command) == 2) {
            Serial.printf("TX NEC addr=0x%02X cmd=0x%02X\n", addr, command);
            tx.sendNec(addr, command);
        } else {
            Serial.println("usage: send <addr> <cmd>");
        }
    } else if (cmd.startsWith("raw ")) {
        unsigned long word = strtoul(cmd.c_str() + 4, nullptr, 0);
        Serial.printf("TX NEC raw=0x%08lX\n", word);
        tx.sendNecRaw(word);
    } else if (cmd == "help" || cmd.length() == 0) {
        Serial.println("commands: send <addr> <cmd> | raw <hex32> | help");
    } else {
        Serial.printf("unknown command: %s\n", cmd.c_str());
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("IR blaster (CB3S / BK7231N) ready");
    Serial.printf("RX pin P%d, TX pin P%d, carrier %d Hz\n", IR_RX_PIN, IR_TX_PIN, IR_CARRIER_HZ);
    Serial.println("Point a remote at the receiver, or type 'help'.");
    rx.begin();
    tx.begin();
}

void loop() {
    // Receive path
    IrNecFrame frame;
    if (rx.poll(frame)) {
        if (frame.repeat) {
            Serial.println("RX NEC repeat");
        } else {
            Serial.printf("RX NEC addr=0x%02X cmd=0x%02X raw=0x%08lX%s\n",
                          frame.address, frame.command, frame.raw,
                          frame.extended ? " (extended)" : "");
        }
    }

    // Serial command path
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            line.trim();
            handleCommand(line);
            line = "";
        } else if (line.length() < 64) {
            line += c;
        }
    }
}
