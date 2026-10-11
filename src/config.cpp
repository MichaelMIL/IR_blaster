#include "config.h"
#include <libretiny.h>

// Stored in the first 4 KiB block of the "userdata" flash partition
// (LibreTiny reserves it for user applications; FLASH_USERDATA_OFFSET comes
// from the board's partition table).

#ifndef IR_TX_PIN
#define IR_TX_PIN 26
#endif
#ifndef IR_RX_PIN
#define IR_RX_PIN 255          // unknown until `rx scan`
#endif
#ifndef STATUS_LED_PIN
#define STATUS_LED_PIN 8       // blue LED on this board
#endif
#ifndef IR_CARRIER_HZ
#define IR_CARRIER_HZ 38000
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
    uint8_t  statusPin;
    uint8_t  statusInvert;
    uint32_t carrierHz;
    uint32_t crc;
};
static constexpr uint32_t CFG_MAGIC   = 0x49524231;   // "IRB1"
static constexpr uint16_t CFG_VERSION = 3;
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
    statusPin = STATUS_LED_PIN;
    statusInvert = false;
    carrierHz = IR_CARRIER_HZ;
}

void DeviceConfig::load() {
    reset();
    StoredConfig s;
    if (lt_flash_read(CFG_OFFSET, (uint8_t *)&s, sizeof(s)) != sizeof(s)) return;
    if (s.magic != CFG_MAGIC) return;
    if (s.version == 2 && s.length == sizeof(s) - sizeof(s.carrierHz)) {
        // v2 layout: same fields without carrierHz, crc right after statusInvert
        struct __attribute__((packed)) V2 { uint32_t magic; uint16_t version, length; char ssid[33]; char pass[65];
                                            uint8_t ledPin[LED_COUNT]; uint8_t rxPin, statusPin, statusInvert; uint32_t crc; } v2;
        lt_flash_read(CFG_OFFSET, (uint8_t *)&v2, sizeof(v2));
        if (v2.crc != crc32((uint8_t *)&v2, sizeof(v2) - 4)) return;
        v2.ssid[32] = 0; v2.pass[64] = 0;
        ssid = v2.ssid; pass = v2.pass;
        for (int i = 0; i < LED_COUNT; i++) ledPin[i] = v2.ledPin[i];
        rxPin = v2.rxPin; statusPin = v2.statusPin; statusInvert = v2.statusInvert != 0;
        Serial.println("config: migrated from v2");
        save();
        return;
    }
    if (s.version != CFG_VERSION || s.length != sizeof(s)) return;
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
    statusPin = s.statusPin;
    statusInvert = s.statusInvert != 0;
    carrierHz = (s.carrierHz >= 30000 && s.carrierHz <= 60000) ? s.carrierHz : IR_CARRIER_HZ;
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
    s.statusPin = statusPin;
    s.statusInvert = statusInvert ? 1 : 0;
    s.carrierHz = carrierHz;
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
