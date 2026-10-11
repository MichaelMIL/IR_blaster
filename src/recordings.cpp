#include "recordings.h"
#include <libretiny.h>
#include <string.h>

static constexpr uint32_t REC_MAGIC = 0x31525249;    // "IRR1"
static constexpr uint32_t BLOCK     = 0x1000;
static constexpr uint32_t BASE      = FLASH_USERDATA_OFFSET + BLOCK;   // block 0 = config

struct __attribute__((packed)) RecHeader {
    uint32_t magic;
    char     name[REC_NAME_LEN + 1];
    uint16_t pulses;
    uint16_t reserved;
    uint32_t crc;          // over the timings
};
static_assert(sizeof(RecHeader) + REC_MAX_PULSES * 4 <= BLOCK, "slot too small");

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFF;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & -(c & 1)); }
    return ~c;
}
static uint32_t slotAddr(int slot) { return BASE + BLOCK * slot; }

static bool readHeader(int slot, RecHeader &h) {
    if (lt_flash_read(slotAddr(slot), (uint8_t *)&h, sizeof(h)) != sizeof(h)) return false;
    if (h.magic != REC_MAGIC || h.pulses == 0 || h.pulses > REC_MAX_PULSES) return false;
    h.name[REC_NAME_LEN] = 0;
    return true;
}

bool recInfo(int slot, RecInfo &info) {
    RecHeader h;
    if (slot < 0 || slot >= REC_SLOTS || !readHeader(slot, h)) return false;
    info.slot = slot;
    strncpy(info.name, h.name, REC_NAME_LEN + 1);
    info.pulses = h.pulses;
    // duration: sum of timings
    uint32_t t[64]; uint32_t sum = 0; int left = h.pulses, off = 0;
    while (left > 0) {
        int n = left < 64 ? left : 64;
        lt_flash_read(slotAddr(slot) + sizeof(RecHeader) + off * 4, (uint8_t *)t, n * 4);
        for (int i = 0; i < n; i++) sum += t[i];
        left -= n; off += n;
    }
    info.durationMs = sum / 1000;
    return true;
}

int recList(RecInfo *out, int max) {
    int n = 0;
    for (int s = 0; s < REC_SLOTS && n < max; s++)
        if (recInfo(s, out[n])) n++;
    return n;
}

int recFind(const char *name) {
    RecHeader h;
    for (int s = 0; s < REC_SLOTS; s++)
        if (readHeader(s, h) && strcmp(h.name, name) == 0) return s;
    return -1;
}

int recLoad(int slot, uint32_t *t, int max) {
    RecHeader h;
    if (slot < 0 || slot >= REC_SLOTS || !readHeader(slot, h)) return 0;
    int n = h.pulses < max ? h.pulses : max;
    if (lt_flash_read(slotAddr(slot) + sizeof(RecHeader), (uint8_t *)t, n * 4) != (uint32_t)(n * 4)) return 0;
    if (n == h.pulses && crc32((uint8_t *)t, n * 4) != h.crc) return 0;
    return n;
}

bool recSave(const char *name, const uint32_t *t, int n) {
    if (!name || !*name || n <= 0 || n > REC_MAX_PULSES) return false;
    int slot = recFind(name);
    if (slot < 0) {
        RecHeader h;
        for (int s = 0; s < REC_SLOTS; s++) if (!readHeader(s, h)) { slot = s; break; }
    }
    if (slot < 0) return false;    // full
    RecHeader h = {};
    h.magic = REC_MAGIC;
    strncpy(h.name, name, REC_NAME_LEN);
    h.pulses = n;
    h.crc = crc32((const uint8_t *)t, n * 4);
    uint32_t a = slotAddr(slot);
    if (!lt_flash_erase_block(a)) return false;
    if (lt_flash_write(a, (uint8_t *)&h, sizeof(h)) != sizeof(h)) return false;
    return lt_flash_write(a + sizeof(h), (const uint8_t *)t, n * 4) == (uint32_t)(n * 4);
}

bool recErase(const char *name) {
    int slot = recFind(name);
    if (slot < 0) return false;
    return lt_flash_erase_block(slotAddr(slot));
}
