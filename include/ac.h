// Native encoder for the mapped air-conditioner remote (see docs/AC_PROTOCOL.md).
#pragma once
#include <Arduino.h>

enum AcMode : uint8_t { AC_AUTO = 0, AC_COOL = 2, AC_DRY = 4, AC_FAN = 6, AC_HEAT = 8 };
enum AcFan  : uint8_t { AC_FAN_AUTO = 0, AC_FAN_HIGH = 1, AC_FAN_MED = 2, AC_FAN_LOW = 3 };
enum AcKey  : uint8_t { AC_KEY_POWER = 0, AC_KEY_MODE = 1, AC_KEY_TEMP_UP = 2, AC_KEY_TEMP_DOWN = 3, AC_KEY_FAN = 5 };

struct AcState {
    bool    power = false;
    uint8_t mode  = AC_COOL;
    uint8_t temp  = 24;      // 16..31
    uint8_t fan   = AC_FAN_AUTO;
};

static constexpr int AC_PULSES = 197;

void   acBuildBytes(const AcState &s, uint8_t key, uint8_t out[12]);
int    acBuildTimings(const AcState &s, uint8_t key, uint32_t *t);   // returns AC_PULSES
bool   acDecodeTimings(const uint32_t *t, int n, AcState &s, uint8_t &key);  // true if a frame of this protocol

const char *acModeName(uint8_t m);    // "auto" "cool" "dry" "fan" "heat"
const char *acFanName(uint8_t f);     // "auto" "high" "med" "low"
bool   acParseMode(const String &s, uint8_t &m);   // accepts HA names too ("fan_only")
bool   acParseFan(const String &s, uint8_t &f);    // accepts "medium"

AcState &acState();                   // last state sent (persisted in config)
bool   acApply(const AcState &s, uint8_t key, String &msg, bool keepRx = false);   // send + remember + publish
String acStateJson();                 // {"power":"on","mode":"cool","temp":24,"fan":"auto"}
