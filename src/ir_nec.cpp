#include "ir_nec.h"

// ---------------------------------------------------------------- receiver

IrNecReceiver *IrNecReceiver::_instance = nullptr;

// NEC timing (µs) and tolerance
static constexpr uint32_t NEC_HDR_MARK   = 9000;
static constexpr uint32_t NEC_HDR_SPACE  = 4500;
static constexpr uint32_t NEC_RPT_SPACE  = 2250;
static constexpr uint32_t NEC_BIT_MARK   = 562;
static constexpr uint32_t NEC_ONE_SPACE  = 1687;
static constexpr uint32_t NEC_ZERO_SPACE = 562;
static constexpr uint32_t NEC_GAP        = 20000;   // silence that ends a frame

static inline bool near(uint32_t v, uint32_t target, uint32_t pct = 30) {
    uint32_t tol = target * pct / 100;
    return v + tol >= target && v <= target + tol;
}

void IrNecReceiver::begin() {
    _instance = this;
    pinMode(_pin, INPUT_PULLUP);          // TSOP-style receivers idle high
    _lastEdge = micros();
    attachInterrupt(digitalPinToInterrupt(_pin), isrTrampoline, CHANGE);
}

void IrNecReceiver::isrTrampoline() {
    if (_instance) _instance->onEdge();
}

void IrNecReceiver::onEdge() {
    uint32_t now = micros();
    uint32_t dt  = now - _lastEdge;
    _lastEdge = now;

    if (_frameReady) return;              // main loop hasn't consumed the last frame yet

    if (dt > NEC_GAP) {                   // long silence: start of a new frame
        _count = 0;
        return;
    }
    if (_count < MAX_EDGES) {
        _edges[_count++] = dt;
    }
    // Full frame = header mark+space + 32 bits * 2 + final mark = 67 entries.
    // Repeat frame = header mark + 2.25ms space + final mark = 3 entries.
    if (_count == 67 || (_count == 3 && near(_edges[1], NEC_RPT_SPACE))) {
        _frameReady = true;
    }
}

bool IrNecReceiver::poll(IrNecFrame &out) {
    if (!_frameReady) return false;
    bool ok = decode(out);
    _count = 0;
    _frameReady = false;
    return ok;
}

bool IrNecReceiver::decode(IrNecFrame &out) {
    // _edges[0] = header mark, _edges[1] = header space, then mark/space pairs.
    if (!near(_edges[0], NEC_HDR_MARK)) return false;

    out = IrNecFrame{};
    if (_count == 3 && near(_edges[1], NEC_RPT_SPACE)) {
        out.repeat = true;
        return true;
    }
    if (_count < 67 || !near(_edges[1], NEC_HDR_SPACE)) return false;

    uint32_t word = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t m = _edges[2 + i * 2];
        uint32_t s = _edges[3 + i * 2];
        if (!near(m, NEC_BIT_MARK, 50)) return false;
        if (near(s, NEC_ONE_SPACE))       word |= (1UL << i);   // LSB first
        else if (!near(s, NEC_ZERO_SPACE)) return false;
    }

    uint8_t a  = word & 0xFF, an = (word >> 8) & 0xFF;
    uint8_t c  = (word >> 16) & 0xFF, cn = (word >> 24) & 0xFF;
    if ((uint8_t)~c != cn) return false;  // command must be followed by its inverse

    out.raw      = word;
    out.address  = a;
    out.command  = c;
    out.extended = ((uint8_t)~a != an);
    return true;
}

// ------------------------------------------------------------------ sender

void IrNecSender::begin() {
    pinMode(_pin, OUTPUT);
    digitalWrite(_pin, LOW);
    _halfPeriodUs = 500000UL / _carrierHz;   // 38 kHz -> 13 µs
}

void IrNecSender::mark(uint32_t us) {
    uint32_t end = micros() + us;
    while ((int32_t)(micros() - end) < 0) {
        digitalWrite(_pin, HIGH);
        delayMicroseconds(_halfPeriodUs);
        digitalWrite(_pin, LOW);
        delayMicroseconds(_halfPeriodUs);
    }
}

void IrNecSender::space(uint32_t us) {
    digitalWrite(_pin, LOW);
    delayMicroseconds(us);
}

void IrNecSender::sendNecRaw(uint32_t word) {
    mark(NEC_HDR_MARK);
    space(NEC_HDR_SPACE);
    for (int i = 0; i < 32; i++) {
        mark(NEC_BIT_MARK);
        space((word >> i) & 1 ? NEC_ONE_SPACE : NEC_ZERO_SPACE);
    }
    mark(NEC_BIT_MARK);
    space(NEC_GAP);
}

void IrNecSender::sendNec(uint8_t address, uint8_t command) {
    uint32_t word = (uint32_t)address
                  | ((uint32_t)(uint8_t)~address << 8)
                  | ((uint32_t)command << 16)
                  | ((uint32_t)(uint8_t)~command << 24);
    sendNecRaw(word);
}

void IrNecSender::sendRepeat() {
    mark(NEC_HDR_MARK);
    space(NEC_RPT_SPACE);
    mark(NEC_BIT_MARK);
    space(NEC_GAP);
}
