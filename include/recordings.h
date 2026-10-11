// Named raw IR recordings stored in flash (one 4 KiB block per slot in the
// "userdata" partition, after the config block).
#pragma once
#include <Arduino.h>

static constexpr int REC_SLOTS     = 32;
static constexpr int REC_NAME_LEN  = 23;
static constexpr int REC_MAX_PULSES = 1000;

struct RecInfo {
    int      slot;
    char     name[REC_NAME_LEN + 1];
    uint16_t pulses;
    uint32_t durationMs;
};

int  recList(RecInfo *out, int max);                       // used slots
int  recFind(const char *name);                            // slot or -1
bool recSave(const char *name, const uint32_t *t, int n);  // overwrites same name
int  recLoad(int slot, uint32_t *t, int max);              // pulses loaded, 0 if empty
bool recErase(const char *name);
bool recInfo(int slot, RecInfo &info);
