// Persistent device configuration (stored in flash via LibreTiny Preferences).
#pragma once
#include <Arduino.h>

static constexpr uint8_t NO_PIN   = 255;
static constexpr int     LED_COUNT = 3;

struct DeviceConfig {
    String  ssid;
    String  pass;
    uint8_t ledPin[LED_COUNT];   // GPIO per IR LED, NO_PIN if unused
    uint8_t rxPin;               // GPIO of the IR receiver output
    uint8_t statusPin;           // GPIO of the status LED (blue), NO_PIN to disable
    bool    statusInvert;        // true if the LED is on when the pin is LOW
    uint32_t carrierHz;          // IR carrier (38000 typical; some ACs use 36000/40000)
    String   mqttHost;           // empty = MQTT disabled
    uint16_t mqttPort;
    String   mqttUser;
    String   mqttPass;

    void load();
    void save() const;
    void reset();

    int  activeLedPins(uint8_t *out) const;   // fills out[], returns count
    bool hasWifi() const { return ssid.length() > 0; }
};

extern DeviceConfig cfg;

// GPIOs that are safe to probe on the CB3S (everything except UART1 P10/P11).
static const uint8_t CANDIDATE_PINS[] = {6, 7, 8, 9, 14, 20, 21, 22, 23, 24, 26};
static constexpr int CANDIDATE_COUNT = sizeof(CANDIDATE_PINS) / sizeof(CANDIDATE_PINS[0]);
bool isCandidatePin(uint8_t pin);
