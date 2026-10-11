#include "buttons.h"
#include "recordings.h"
#include <libretiny.h>
#include <string.h>

static constexpr uint32_t BTN_MAGIC = 0x314E5442;   // "BTN1"
static constexpr uint32_t BLOCK     = 0x1000;
static constexpr uint32_t ADDR      = FLASH_USERDATA_OFFSET + BLOCK * (1 + REC_SLOTS);   // block 33

struct __attribute__((packed)) Store {
    uint32_t magic;
    uint8_t  count;
    uint8_t  reserved[3];
    Button   b[BTN_MAX];
    uint32_t crc;
};
static_assert(sizeof(Store) <= BLOCK, "button store too big");

static Store store;
static bool  loaded = false;

static uint32_t crc32(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFF;
    while (n--) { c ^= *p++; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & -(c & 1)); }
    return ~c;
}

void btnLoad() {
    loaded = true;
    if (lt_flash_read(ADDR, (uint8_t *)&store, sizeof(store)) != sizeof(store) ||
        store.magic != BTN_MAGIC || store.count > BTN_MAX ||
        store.crc != crc32((uint8_t *)&store, sizeof(store) - 4)) {
        memset(&store, 0, sizeof(store));
        store.magic = BTN_MAGIC;
    }
}

static bool save() {
    store.crc = crc32((uint8_t *)&store, sizeof(store) - 4);
    if (!lt_flash_erase_block(ADDR)) return false;
    return lt_flash_write(ADDR, (uint8_t *)&store, sizeof(store)) == sizeof(store);
}

int btnCount() { if (!loaded) btnLoad(); return store.count; }
const Button *btnGet(int i) { if (!loaded) btnLoad(); return (i >= 0 && i < store.count) ? &store.b[i] : nullptr; }

bool btnSet(int i, const Button &b) {
    if (!loaded) btnLoad();
    if (i < 0 || i > store.count || i >= BTN_MAX) return false;
    store.b[i] = b;
    store.b[i].label[BTN_LABEL_LEN] = 0;
    store.b[i].rec[BTN_REC_LEN] = 0;
    if (store.b[i].repeat < 1) store.b[i].repeat = 1;
    if (store.b[i].repeat > 5) store.b[i].repeat = 5;
    if (i == store.count) store.count++;
    return save();
}

bool btnDelete(int i) {
    if (!loaded) btnLoad();
    if (i < 0 || i >= store.count) return false;
    for (int k = i; k + 1 < store.count; k++) store.b[k] = store.b[k + 1];
    store.count--;
    return save();
}

bool btnMove(int i, int dir) {
    if (!loaded) btnLoad();
    int j = i + dir;
    if (i < 0 || i >= store.count || j < 0 || j >= store.count) return false;
    Button t = store.b[i]; store.b[i] = store.b[j]; store.b[j] = t;
    return save();
}
