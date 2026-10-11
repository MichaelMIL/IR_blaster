// Functions shared between the serial console and the web UI.
#pragma once
#include <Arduino.h>
#include "config.h"
#include "ir_nec.h"

struct RxRecord {
    uint32_t t;         // millis() when received
    uint8_t  type;      // 0 raw, 1 nec, 2 repeat
    uint8_t  address;
    uint8_t  command;
    uint32_t raw;
    uint16_t pulses;
};
static constexpr int RX_HISTORY = 20;

struct ScanResult {
    uint32_t transitions[CANDIDATE_COUNT];
    uint8_t  idle[CANDIDATE_COUNT];
    int      best;      // index into CANDIDATE_PINS, -1 if none
};

int      appGetReceived(RxRecord *out, int max);   // oldest first
uint32_t appReceivedSeq();                         // increments per frame
void     appClearReceived();

bool appSendNec(uint8_t addr, uint8_t cmd, bool keepRx = false);   // keepRx: receiver stays on (self-test)
bool appSendRaw(uint32_t word);
int  appTxPinCount();

void appLedTest(int idx, uint32_t seconds);        // idx -1 = all configured
void appBlinkPin(uint8_t pin, uint32_t seconds);   // any candidate pin, config untouched
void appRxScan(uint32_t seconds, ScanResult &r);
void appApplyPins();

void   appWifiBegin();
bool   appWifiConnected();
String appIp();

// Recordings
int    appRecord(const char *name, uint32_t waitSec, String &msg);   // waits for next IR frame
bool   appSaveLast(const char *name, String &msg);                    // saves the last capture
bool   appPlay(const char *name, String &msg);
int    appLastPulses();
const uint32_t *appLastCapture();

bool appPressButton(int i, String &msg);

// Live capture stream: the last few raw captures with full timings
static constexpr int STREAM_CAPS = 8;
struct StreamCap {
    uint32_t id;            // 0 = empty; increments per capture
    uint32_t t;             // millis()
    uint8_t  type;          // 0 raw, 1 nec, 2 repeat
    uint8_t  address, command;
    uint32_t raw;
    uint16_t n;
    uint16_t v[IrReceiver::MAX_EDGES];
};
uint32_t appStreamLastId();
const StreamCap *appStreamGet(int i);              // i = 0..STREAM_CAPS-1, oldest first; nullptr if empty
bool appStreamSave(uint32_t id, const char *name, String &msg);
void appStreamClear();

// Status LED
void appStatusBlink(uint32_t ms);
void appApplyStatusLed();

void webBegin();
void webLoop();
