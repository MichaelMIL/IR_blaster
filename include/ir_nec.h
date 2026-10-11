// Dependency-free IR capture + NEC encoder/decoder for LibreTiny (BK7231N).
//
// Receiver: interrupt driven. Every edge on the RX pin is timestamped into a
// buffer; a frame ends on >80 ms of silence or when the buffer is full. The
// 80 ms keeps multi-part AC frames (gaps of 10-40 ms) together as one capture
// while NEC repeats (98 ms apart) stay separate.
// Frames are decoded as NEC when possible, otherwise exposed as raw timings.
//
// Sender: bit-bangs a carrier on up to 3 GPIOs at once.

#pragma once
#include <Arduino.h>

struct IrFrame {
    bool     decoded;   // true if a valid NEC frame
    bool     repeat;    // NEC repeat code
    bool     extended;  // address byte not followed by its inverse
    uint8_t  address;
    uint8_t  command;
    uint32_t raw;       // 32-bit NEC word, LSB first
    int      pulses;    // number of raw timings captured
};

class IrReceiver {
public:
    static constexpr int MAX_EDGES = 800;   // enough for multi-part AC frames
    static constexpr int MIN_PULSES = 6;    // shorter captures are ambient-light glitches

    void begin(uint8_t pin);
    void end();
    bool active() const { return _pin != 255; }
    uint8_t pin() const { return _pin; }

    // true when a frame is ready; optionally copies its raw timings (µs, mark first)
    bool poll(IrFrame &out, uint32_t *timings = nullptr, int max = 0);
    uint32_t edgeCount() const { return _totalEdges; }
    uint32_t noiseCount() const { return _noise; }   // glitch frames dropped

private:
    static void isrTrampoline();
    void onEdge();
    bool decodeNec(IrFrame &out) const;

    static IrReceiver *_instance;
    uint8_t _pin = 255;
    volatile uint32_t _edges[MAX_EDGES];
    volatile int      _count = 0;
    volatile uint32_t _lastEdge = 0;
    volatile bool     _frameReady = false;
    volatile uint32_t _totalEdges = 0;
    uint32_t _noise = 0;
};

// Merge receiver dropouts in a captured timing list: any mark or space shorter
// than minUs is joined with its neighbours. Returns the new length.
int irCleanGlitches(uint32_t *t, int n, uint32_t minUs = 250);

class IrSender {
public:
    static constexpr int MAX_PINS = 3;

    void setPins(const uint8_t *pins, int count);
    void setCarrier(uint32_t hz);
    void setPwm(bool usePwm);              // hardware PWM carrier (default) or bit-bang
    bool pwm() const { return _pwm; }
    int  pinCount() const { return _count; }
    uint32_t measureBitbangHz();           // actual bit-bang carrier frequency, for diagnostics

    void sendTimings(const uint32_t *t, int n);   // raw mark/space list, mark first
    void sendNec(uint8_t address, uint8_t command);
    void sendNecRaw(uint32_t word);
    void sendRepeat();

    // Carrier burst for testing: `onMs` of carrier, `offMs` of silence, repeated.
    void burst(uint32_t onMs, uint32_t offMs, uint32_t totalMs);

private:
    void mark(uint32_t us);
    void space(uint32_t us);
    void writeAll(int level);
    void calibrate();

    void pinsIdle();
    void pinsCarrier(bool on);

    uint8_t  _pins[MAX_PINS];
    bool     _pwmOk[MAX_PINS];
    int      _count = 0;
    bool     _pwm = true;
    uint32_t _carrierHz = 38000;
    uint32_t _halfPeriodUs = 13;
    uint32_t _writeOverheadUs = 0;
};
