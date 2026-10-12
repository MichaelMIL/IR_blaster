// MQTT client with Home Assistant MQTT discovery.
//
// Topics (id = last 3 bytes of the MAC, e.g. ir_blaster/a1b2c3):
//   <base>/status        online | offline (retained, LWT)
//   <base>/received      JSON per received frame
//   <base>/cmd/button    payload: button index or label      -> press a remote button
//   <base>/cmd/send      payload: "0xBA 0x1E" or {"addr":..,"cmd":..}
//   <base>/cmd/raw       payload: 32-bit NEC word, e.g. 0xE11E45BA
//   <base>/cmd/play      payload: recording name
// Discovery: one HA `button` per remote button, one `sensor` with the last code.
#pragma once
#include <Arduino.h>
#include "app.h"

void   mqttBegin();
void   mqttLoop();
void   mqttPublishReceived(const RxRecord &r);
void   mqttPublishDiscovery();     // call after buttons change
void   mqttPublishAcState();
void   mqttReconfigure();          // call after config change
void   mqttSuspend();              // disconnect and stay off (used during OTA)
void   mqttResume();
String mqttState();                // "disabled" | "connecting" | "connected" | "error ..."
String mqttBaseTopic();
