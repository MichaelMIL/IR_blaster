#include "config.h"
#include <libretiny.h>

// Stored in the first 4 KiB block of the "userdata" flash partition
// (LibreTiny reserves it for user applications; FLASH_USERDATA_OFFSET comes
// from the board's partition table).

#ifndef IR_TX_PIN
#define IR_TX_PIN 26
#endif
#ifndef IR_RX_PIN
#define IR_RX_PIN 8
#endif
#ifndef FLASH_USERDATA_OFFSET
#error "FLASH_USERDATA_OFFSET not defined by the board"
#endif

DeviceConfig cfg;

struct __attribute__((packed)) StoredConfig {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    char     ssid[33];
    char     pass[65];
    uint8_t  ledPin[LED_COUNT];
    uint8_t  rxPin;
    uint32_t crc;
};
static constexpr uint32_t CFG_MAGIC   = 0x49524231;   // "IRB1"
static constexpr uint16_t CFG_VERSION = 1;
static constexpr uint32_t CFG_OFFSET  = FLASH_USERDATA_OFFSET;

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFF;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
    }
    return ~c;
}

void DeviceConfig::reset() {
    ssid = "";
    pass = "";
    ledPin[0] = IR_TX_PIN;
    for (int i = 1; i < LED_COUNT; i++) ledPin[i] = NO_PIN;
    rxPin = IR_RX_PIN;
}

void DeviceConfig::load() {
    reset();
    StoredConfig s;
    if (lt_flash_read(CFG_OFFSET, (uint8_t *)&s, sizeof(s)) != sizeof(s)) return;
    if (s.magic != CFG_MAGIC || s.version != CFG_VERSION || s.length != sizeof(s)) return;
    if (s.crc != crc32((uint8_t *)&s, sizeof(s) - sizeof(s.crc))) {
        Serial.println("config: bad checksum, using defaults");
        return;
    }
    s.ssid[sizeof(s.ssid) - 1] = 0;
    s.pass[sizeof(s.pass) - 1] = 0;
    ssid = s.ssid;
    pass = s.pass;
    for (int i = 0; i < LED_COUNT; i++) ledPin[i] = s.ledPin[i];
    rxPin = s.rxPin;
}

void DeviceConfig::save() const {
    StoredConfig s = {};
    s.magic   = CFG_MAGIC;
    s.version = CFG_VERSION;
    s.length  = sizeof(s);
    strncpy(s.ssid, ssid.c_str(), sizeof(s.ssid) - 1);
    strncpy(s.pass, pass.c_str(), sizeof(s.pass) - 1);
    for (int i = 0; i < LED_COUNT; i++) s.ledPin[i] = ledPin[i];
    s.rxPin = rxPin;
    s.crc = crc32((uint8_t *)&s, sizeof(s) - sizeof(s.crc));

    if (!lt_flash_erase_block(CFG_OFFSET)) { Serial.println("config: erase failed"); return; }
    if (lt_flash_write(CFG_OFFSET, (uint8_t *)&s, sizeof(s)) != sizeof(s)) {
        Serial.println("config: write failed");
    }
}

int DeviceConfig::activeLedPins(uint8_t *out) const {
    int n = 0;
    for (int i = 0; i < LED_COUNT; i++)
        if (ledPin[i] != NO_PIN) out[n++] = ledPin[i];
    return n;
}

bool isCandidatePin(uint8_t pin) {
    for (int i = 0; i < CANDIDATE_COUNT; i++)
        if (CANDIDATE_PINS[i] == pin) return true;
    return false;
}
