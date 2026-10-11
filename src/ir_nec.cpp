#include "ir_nec.h"
#include <FreeRTOS.h>
#include <task.h>

// NEC timing (µs)
static constexpr uint32_t NEC_HDR_MARK   = 9000;
static constexpr uint32_t NEC_HDR_SPACE  = 4500;
static constexpr uint32_t NEC_RPT_SPACE  = 2250;
static constexpr uint32_t NEC_BIT_MARK   = 562;
static constexpr uint32_t NEC_ONE_SPACE  = 1687;
static constexpr uint32_t NEC_ZERO_SPACE = 562;
static constexpr uint32_t FRAME_GAP      = 80000;   // silence that ends a capture
static constexpr uint32_t TX_GAP         = 40000;   // silence appended after a transmission

static inline bool near(uint32_t v, uint32_t target, uint32_t pct = 30) {
    uint32_t tol = target * pct / 100;
    return v + tol >= target && v <= target + tol;
}

// ---------------------------------------------------------------- receiver

IrReceiver *IrReceiver::_instance = nullptr;

void IrReceiver::begin(uint8_t pin) {
    end();
    _pin = pin;
    _instance = this;
    _count = 0;
    _frameReady = false;
    pinMode(_pin, INPUT_PULLUP);          // TSOP-style receivers idle high
    _lastEdge = micros();
    attachInterrupt(digitalPinToInterrupt(_pin), isrTrampoline, CHANGE);
}

void IrReceiver::end() {
    if (_pin == 255) return;
    detachInterrupt(digitalPinToInterrupt(_pin));
    pinMode(_pin, INPUT);
    _pin = 255;
}

void IrReceiver::isrTrampoline() {
    if (_instance) _instance->onEdge();
}

void IrReceiver::onEdge() {
    uint32_t now = micros();
    uint32_t dt  = now - _lastEdge;
    _lastEdge = now;
    _totalEdges++;

    if (_frameReady) return;              // previous frame not consumed yet: drop

    if (dt > FRAME_GAP) {                 // long silence: this edge starts a new frame
        _gapPending = dt;
        if (_count > 0) { _frameReady = true; return; }
        _count = 0;
        return;
    }
    if (_count < MAX_EDGES) _edges[_count++] = dt;
    if (_count == MAX_EDGES) _frameReady = true;
}

bool IrReceiver::poll(IrFrame &out, uint32_t *timings, int max) {
    if (_pin == 255) return false;
    // Frame ended by silence but no new edge has arrived yet to flag it.
    if (!_frameReady && _count > 0 && (micros() - _lastEdge) > FRAME_GAP) {
        _frameReady = true;
    }
    if (!_frameReady) return false;
    if (_count < MIN_PULSES) {            // glitch: discard silently
        _noise++;
        _count = 0;
        _frameReady = false;
        return false;
    }

    _gapBefore = _gapPending;
    out = IrFrame{};
    out.pulses  = _count;
    out.decoded = decodeNec(out);
    if (timings) { int n = _count < max ? _count : max; for (int i = 0; i < n; i++) timings[i] = _edges[i]; }
    _count = 0;
    _frameReady = false;
    return true;
}

bool IrReceiver::decodeNec(IrFrame &out) const {
    // _edges[0] = header mark, _edges[1] = header space, then mark/space pairs.
    if (_count < 3 || !near(_edges[0], NEC_HDR_MARK)) return false;

    if (_count <= 4 && near(_edges[1], NEC_RPT_SPACE)) {
        out.repeat = true;
        return true;
    }
    if (_count < 66 || !near(_edges[1], NEC_HDR_SPACE)) return false;

    uint32_t word = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t m = _edges[2 + i * 2];
        uint32_t s = (3 + i * 2 < _count) ? _edges[3 + i * 2] : NEC_ZERO_SPACE;
        if (!near(m, NEC_BIT_MARK, 50)) return false;
        if (near(s, NEC_ONE_SPACE))        word |= (1UL << i);   // LSB first
        else if (!near(s, NEC_ZERO_SPACE)) return false;
    }

    uint8_t a  = word & 0xFF, an = (word >> 8) & 0xFF;
    uint8_t c  = (word >> 16) & 0xFF, cn = (word >> 24) & 0xFF;
    if ((uint8_t)~c != cn) return false;

    out.raw      = word;
    out.address  = a;
    out.command  = c;
    out.extended = ((uint8_t)~a != an);
    return true;
}

int irCleanGlitches(uint32_t *t, int n, uint32_t minUs) {
    int i = 1;
    while (i < n - 1) {
        if (t[i] < minUs) {
            t[i - 1] = t[i - 1] + t[i] + t[i + 1];
            for (int k = i; k + 2 < n; k++) t[k] = t[k + 2];
            n -= 2;
            if (i > 1) i--;
        } else {
            i++;
        }
    }
    return n;
}

// ------------------------------------------------------------------ sender

// PWM-capable GPIOs on the BK7231N (CB3S): P6 P7 P8 P9 P24 P26
static bool pinHasPwm(uint8_t p) { return p == 6 || p == 7 || p == 8 || p == 9 || p == 24 || p == 26; }


void IrSender::setPins(const uint8_t *pins, int count) {
    pinsIdle();
    for (int i = 0; i < _count; i++) pinMode(_pins[i], INPUT);   // release old pins
    _count = count > MAX_PINS ? MAX_PINS : count;
    for (int i = 0; i < _count; i++) {
        _pins[i]  = pins[i];
        _pwmOk[i] = _pwm && pinHasPwm(pins[i]);
        if (_pwmOk[i]) {
            analogWritePeriod(1000000UL / _carrierHz);   // 38 kHz -> 26 us (hardware: 38.46 kHz)
            analogWrite(_pins[i], 0);                    // claim the pin for PWM, output idle
        } else {
            pinMode(_pins[i], OUTPUT);
            digitalWrite(_pins[i], LOW);
        }
    }
    calibrate();
}

void IrSender::setPwm(bool usePwm) {
    uint8_t pins[MAX_PINS]; int n = _count;
    for (int i = 0; i < n; i++) pins[i] = _pins[i];
    _pwm = usePwm;
    setPins(pins, n);
}

void IrSender::setCarrier(uint32_t hz) {
    _carrierHz = hz;
    uint8_t pins[MAX_PINS]; int n = _count;
    for (int i = 0; i < n; i++) pins[i] = _pins[i];
    setPins(pins, n);                                // re-applies the PWM period
}

void IrSender::calibrate() {
    _halfPeriodUs = 500000UL / _carrierHz;          // 38 kHz -> 13 us
    _writeOverheadUs = 0;
    if (_count == 0) return;
    // Measure how long a GPIO write of all bit-banged pins takes so the
    // software carrier stays on frequency regardless of LED count.
    uint32_t t0 = micros();
    for (int i = 0; i < 200; i++) writeAll(LOW);
    uint32_t perCall = (micros() - t0) / 200;
    _writeOverheadUs = perCall < _halfPeriodUs - 2 ? perCall : _halfPeriodUs - 2;

    // Measure the PWM start/stop call cost so it can be subtracted from each
    // mark/space (it is ~100 us on the BDK driver, which otherwise stretches
    // every pulse). Emits a few ms of carrier; harmless.
    bool anyPwm = false;
    for (int i = 0; i < _count; i++) if (_pwmOk[i]) anyPwm = true;
    _pwmOnOverheadUs = _pwmOffOverheadUs = 0;
    if (anyPwm) {
        uint32_t on = 0, off = 0;
        for (int i = 0; i < 20; i++) {
            uint32_t a = micros(); pinsCarrier(true);  uint32_t b = micros(); pinsCarrier(false); uint32_t c = micros();
            on += b - a; off += c - b;
        }
        _pwmOnOverheadUs = on / 20;
        _pwmOffOverheadUs = off / 20;
        // Defaults from loopback measurements on the CB3S: the stop call
        // lengthens the mark it ends, the start call lengthens the space it ends.
        _markCompUs  = _pwmOffOverheadUs;
        _spaceCompUs = _pwmOnOverheadUs;
    }
}

uint32_t IrSender::measureBitbangHz() {
    if (_count == 0) return 0;
    uint32_t half = _halfPeriodUs - _writeOverheadUs;
    const int cycles = 2000;
    uint32_t t0 = micros();
    for (int i = 0; i < cycles; i++) {
        writeAll(LOW); delayMicroseconds(half);     // LOW both halves: measures timing only, no IR emitted
        writeAll(LOW); delayMicroseconds(half);
    }
    uint32_t total = micros() - t0;
    return total ? (uint32_t)((uint64_t)cycles * 1000000ULL / total) : 0;
}

void IrSender::writeAll(int level) {
    for (int i = 0; i < _count; i++)
        if (!_pwmOk[i]) digitalWrite(_pins[i], level);
}

void IrSender::pinsIdle() {
    for (int i = 0; i < _count; i++) {
        if (_pwmOk[i]) analogWrite(_pins[i], 0);
        else digitalWrite(_pins[i], LOW);
    }
}

void IrSender::pinsCarrier(bool on) {
    // 50 % duty during a mark, PWM paused (output low) during a space.
    // (Keeping the PWM running at a tiny duty instead was tried: the BDK
    // driver then leaves the output high, so the LED stays lit.)
    for (int i = 0; i < _count; i++)
        if (_pwmOk[i]) analogWrite(_pins[i], on ? 128 : 0);
}

void IrSender::mark(uint32_t us) {
    bool anyBitbang = false;
    for (int i = 0; i < _count; i++) if (!_pwmOk[i]) anyBitbang = true;
    pinsCarrier(true);
    if (!anyBitbang) {
        int32_t adj = (int32_t)us - _markCompUs;
        us = adj > 20 ? (uint32_t)adj : 20;
        while (us > 10000) { delayMicroseconds(10000); us -= 10000; }
        delayMicroseconds(us);
    } else {
        uint32_t half = _halfPeriodUs - _writeOverheadUs;
        uint32_t end = micros() + us;
        while ((int32_t)(micros() - end) < 0) {
            writeAll(HIGH);
            delayMicroseconds(half);
            writeAll(LOW);
            delayMicroseconds(half);
        }
    }
    pinsCarrier(false);
}

void IrSender::space(uint32_t us) {
    writeAll(LOW);
    int32_t adj = (int32_t)us - _spaceCompUs;
    us = adj > 20 ? (uint32_t)adj : 20;
    while (us > 10000) { delayMicroseconds(10000); us -= 10000; }
    delayMicroseconds(us);
}

void IrSender::sendTimings(const uint32_t *t, int n) {
    if (_count == 0 || n <= 0) return;
    // Run at top priority while transmitting so the WiFi task cannot preempt
    // us mid-pulse (seen as random multi-ms stretches of single spaces).
    // (Suspending the scheduler instead broke the PWM driver and crashed.)
    UBaseType_t prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, configMAX_PRIORITIES - 1);
    for (int i = 0; i < n; i++) {
        if (i & 1) space(t[i]); else mark(t[i]);
    }
    pinsIdle();
    vTaskPrioritySet(NULL, prio);
    space(TX_GAP);
}

void IrSender::sendNecRaw(uint32_t word) {
    if (_count == 0) return;
    uint32_t t[67]; int k = 0;
    t[k++] = NEC_HDR_MARK; t[k++] = NEC_HDR_SPACE;
    for (int i = 0; i < 32; i++) { t[k++] = NEC_BIT_MARK; t[k++] = (word >> i) & 1 ? NEC_ONE_SPACE : NEC_ZERO_SPACE; }
    t[k++] = NEC_BIT_MARK;
    sendTimings(t, k);
}

void IrSender::sendNec(uint8_t address, uint8_t command) {
    uint32_t word = (uint32_t)address
                  | ((uint32_t)(uint8_t)~address << 8)
                  | ((uint32_t)command << 16)
                  | ((uint32_t)(uint8_t)~command << 24);
    sendNecRaw(word);
}

void IrSender::sendRepeat() {
    if (_count == 0) return;
    uint32_t t[3] = {NEC_HDR_MARK, NEC_RPT_SPACE, NEC_BIT_MARK};
    sendTimings(t, 3);
}

void IrSender::burst(uint32_t onMs, uint32_t offMs, uint32_t totalMs) {
    if (_count == 0) return;
    uint32_t end = millis() + totalMs;
    while ((int32_t)(millis() - end) < 0) {
        mark(onMs * 1000);
        space(offMs * 1000);
    }
}
