#include "ac.h"
#include "app.h"
#include "config.h"
#include "mqtt.h"

static constexpr uint32_t HDR_MARK = 6000, HDR_SPACE = 7300, BIT_MARK = 560, ZERO = 530, ONE = 1590, END_GAP = 7300;

void acBuildBytes(const AcState &s, uint8_t key, uint8_t b[12]) {
    for (int i = 0; i < 12; i++) b[i] = 0;
    b[3]  = 0x01 | (s.mode == AC_HEAT ? 0x20 : 0);
    b[5]  = key;
    b[7]  = (s.power ? 0x02 : 0) | 0x08 | ((s.fan & 3) << 5);
    b[9]  = ((s.mode & 0x0F) << 4) | ((s.temp - 16) & 0x0F);
    b[10] = 0xA5;
    uint32_t sum = 0;
    for (int i = 0; i < 11; i++) sum += b[i];
    b[11] = sum & 0xFF;
}

int acBuildTimings(const AcState &s, uint8_t key, uint32_t *t) {
    uint8_t b[12];
    acBuildBytes(s, key, b);
    int n = 0;
    t[n++] = HDR_MARK; t[n++] = HDR_SPACE;
    for (int i = 0; i < 12; i++)
        for (int bit = 0; bit < 8; bit++) {            // LSB first
            t[n++] = BIT_MARK;
            t[n++] = (b[i] >> bit) & 1 ? ONE : ZERO;
        }
    t[n++] = BIT_MARK; t[n++] = END_GAP; t[n++] = BIT_MARK;
    return n;
}

bool acDecodeTimings(const uint32_t *t, int n, AcState &s, uint8_t &key) {
    if (n < 2 + 192 || t[0] < 4500 || t[0] > 7500 || t[1] < 5500 || t[1] > 9000) return false;
    uint8_t b[12] = {0};
    for (int i = 0; i < 96; i++)
        if (t[3 + i * 2] > 1000) b[i / 8] |= 1 << (i % 8);
    uint32_t sum = 0;
    for (int i = 0; i < 11; i++) sum += b[i];
    if (b[10] != 0xA5 || b[11] != (sum & 0xFF)) return false;
    s.power = b[7] & 0x02;
    s.fan   = (b[7] >> 5) & 3;
    s.mode  = b[9] >> 4;
    s.temp  = 16 + (b[9] & 0x0F);
    key     = b[5];
    return true;
}

const char *acModeName(uint8_t m) {
    switch (m) { case AC_AUTO: return "auto"; case AC_COOL: return "cool"; case AC_DRY: return "dry"; case AC_FAN: return "fan"; case AC_HEAT: return "heat"; default: return "?"; }
}
const char *acFanName(uint8_t f) {
    switch (f) { case AC_FAN_AUTO: return "auto"; case AC_FAN_HIGH: return "high"; case AC_FAN_MED: return "med"; case AC_FAN_LOW: return "low"; default: return "?"; }
}
bool acParseMode(const String &v, uint8_t &m) {
    String s = v; s.toLowerCase();
    if (s == "auto") m = AC_AUTO; else if (s == "cool") m = AC_COOL; else if (s == "dry") m = AC_DRY;
    else if (s == "fan" || s == "fan_only") m = AC_FAN; else if (s == "heat") m = AC_HEAT; else return false;
    return true;
}
bool acParseFan(const String &v, uint8_t &f) {
    String s = v; s.toLowerCase();
    if (s == "auto") f = AC_FAN_AUTO; else if (s == "high") f = AC_FAN_HIGH; else if (s == "med" || s == "medium") f = AC_FAN_MED;
    else if (s == "low") f = AC_FAN_LOW; else return false;
    return true;
}

static AcState current;
static bool    loaded = false;

AcState &acState() {
    if (!loaded) {
        loaded = true;
        current.power = cfg.acPower; current.mode = cfg.acMode; current.temp = cfg.acTemp; current.fan = cfg.acFan;
        if (current.temp < 16 || current.temp > 31) current.temp = 24;
    }
    return current;
}

String acStateJson() {
    const AcState &s = acState();
    return String("{\"power\":\"") + (s.power ? "on" : "off") + "\",\"mode\":\"" + acModeName(s.mode) +
           "\",\"temp\":" + String(s.temp) + ",\"fan\":\"" + acFanName(s.fan) + "\",\"hvac_mode\":\"" +
           (s.power ? (s.mode == AC_FAN ? "fan_only" : acModeName(s.mode)) : "off") + "\",\"fan_mode\":\"" +
           (s.fan == AC_FAN_MED ? "medium" : acFanName(s.fan)) + "\"}";
}

bool acApply(const AcState &s, uint8_t key, String &msg, bool keepRx) {
    if (s.temp < 16 || s.temp > 31) { msg = "temp must be 16..31"; return false; }
    uint32_t *t = appScratch();
    int n = acBuildTimings(s, key, t);
    if (!appSendTimings(t, n, keepRx)) { msg = "no LED pins assigned"; return false; }
    acState() = s;
    cfg.acPower = s.power; cfg.acMode = s.mode; cfg.acTemp = s.temp; cfg.acFan = s.fan;
    cfg.save();
    mqttPublishAcState();
    msg = String("AC ") + (s.power ? "on, " : "off (") + acModeName(s.mode) + " " + String(s.temp) + "C fan " + acFanName(s.fan) + (s.power ? "" : ")");
    return true;
}
