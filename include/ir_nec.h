// Small, dependency-free NEC IR encoder/decoder for LibreTiny (BK7231N).
//
// The receiver is interrupt driven: every edge on the RX pin is timestamped
// and pushed into a ring buffer, which loop() drains and decodes.
// The sender bit-bangs a carrier with digitalWrite()/delayMicroseconds().

#pragma once
#include <Arduino.h>

struct IrNecFrame {
    uint8_t  address;   // 8-bit address (low byte for extended NEC)
    uint8_t  command;
    uint32_t raw;       // full 32-bit word as transmitted, LSB first
    bool     extended;  // address byte was not followed by its inverse
    bool     repeat;    // NEC repeat code (9ms mark, 2.25ms space)
};

class IrNecReceiver {
public:
    explicit IrNecReceiver(uint8_t pin) : _pin(pin) {}
    void begin();
    bool poll(IrNecFrame &out);

private:
    static void isrTrampoline();
    void onEdge();
    bool decode(IrNecFrame &out);

    static IrNecReceiver *_instance;
    uint8_t _pin;

    static constexpr int MAX_EDGES = 80;
    volatile uint32_t _edges[MAX_EDGES];   // duration of each pulse/gap in µs
    volatile int      _count = 0;
    volatile uint32_t _lastEdge = 0;
    volatile bool     _frameReady = false;
};

class IrNecSender {
public:
    IrNecSender(uint8_t pin, uint32_t carrierHz) : _pin(pin), _carrierHz(carrierHz) {}
    void begin();
    void sendNec(uint8_t address, uint8_t command);
    void sendNecRaw(uint32_t word);
    void sendRepeat();

private:
    void mark(uint32_t us);
    void space(uint32_t us);

    uint8_t  _pin;
    uint32_t _carrierHz;
    uint32_t _halfPeriodUs = 13;
};
