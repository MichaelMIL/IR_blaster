// User-facing remote buttons: label + action, stored in flash (one block in
// the userdata partition, after the config and recording slots).
#pragma once
#include <Arduino.h>

static constexpr int BTN_MAX       = 24;
static constexpr int BTN_LABEL_LEN = 16;
static constexpr int BTN_REC_LEN   = 23;

enum ButtonType : uint8_t { BTN_NEC = 0, BTN_REC = 1 };

struct __attribute__((packed)) Button {
    char    label[BTN_LABEL_LEN + 1];
    uint8_t type;        // ButtonType
    uint8_t addr, cmd;   // NEC
    char    rec[BTN_REC_LEN + 1];   // recording name
    uint8_t repeat;      // how many times to send (1..5)
};

int  btnCount();
const Button *btnGet(int i);
bool btnSet(int i, const Button &b);   // i == count appends; returns false if full
bool btnDelete(int i);
bool btnMove(int i, int dir);          // dir -1 up, +1 down
void btnLoad();
