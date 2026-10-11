// Functions shared between the serial console and the web UI.
#pragma once
#include <Arduino.h>
#include "config.h"

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

bool appSendNec(uint8_t addr, uint8_t cmd);
bool appSendRaw(uint32_t word);
int  appTxPinCount();

void appLedTest(int idx, uint32_t seconds);        // idx -1 = all configured
void appBlinkPin(uint8_t pin, uint32_t seconds);   // any candidate pin, config untouched
void appRxScan(uint32_t seconds, ScanResult &r);
void appApplyPins();

void   appWifiBegin();
bool   appWifiConnected();
String appIp();

void webBegin();
void webLoop();
