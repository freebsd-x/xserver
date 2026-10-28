#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// 5-band equalizer for com.freebsdx.AudioEq, applied by AudioService to the
// mixed stereo stream before it goes to /dev/dsp. RBJ-cookbook biquads: low
// shelf 80 Hz, peaks at 400 Hz / 1 kHz / 3.5 kHz, high shelf 10 kHz.
// Dependency-free / unit-testable.
namespace fbx::xserver::eq {

constexpr int kBands = 5;
constexpr int kMaxGainDb = 12;
using Gains = std::array<int8_t, kBands>;   // Bass, Low-mid, Mid, High-mid, Treble (dB)

enum Preset : uint8_t { kFlat = 0, kBassBoost = 1, kVocal = 2, kGaming = 3, kCustom = 4 };

inline Gains presetGains(uint8_t preset) {
    switch (preset) {
    case kBassBoost: return {6, 3, 0, 0, 0};
    case kVocal:     return {-2, -1, 3, 4, 1};
    case kGaming:    return {3, 0, -1, 3, 4};
    default:         return {0, 0, 0, 0, 0};
    }
}

inline Gains clampGains(Gains g) {
    for (auto& v : g) v = static_cast<int8_t>(v < -kMaxGainDb ? -kMaxGainDb : v > kMaxGainDb ? kMaxGainDb : v);
    return g;
}

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
};

enum class Shape { LowShelf, Peak, HighShelf };

// RBJ Audio EQ Cookbook (Q = 1 for peaks, shelf slope S = 1).
inline Biquad design(Shape shape, double f0, double gain_db, double rate) {
    Biquad c;
    if (gain_db == 0.0) return c;
    const double A = std::pow(10.0, gain_db / 40.0);
    const double w0 = 2.0 * M_PI * f0 / rate;
    const double cw = std::cos(w0), sw = std::sin(w0);
    double b0, b1, b2, a0, a1, a2;
    if (shape == Shape::Peak) {
        const double alpha = sw / 2.0;   // Q = 1
        b0 = 1 + alpha * A; b1 = -2 * cw; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * cw; a2 = 1 - alpha / A;
    } else {
        const double alpha = sw / 2.0 * std::sqrt(2.0);   // S = 1
        const double sa = 2 * std::sqrt(A) * alpha;
        if (shape == Shape::LowShelf) {
            b0 = A * ((A + 1) - (A - 1) * cw + sa);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - sa);
            a0 = (A + 1) + (A - 1) * cw + sa;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - sa;
        } else {
            b0 = A * ((A + 1) + (A - 1) * cw + sa);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - sa);
            a0 = (A + 1) - (A - 1) * cw + sa;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - sa;
        }
    }
    c.b0 = b0 / a0; c.b1 = b1 / a0; c.b2 = b2 / a0; c.a1 = a1 / a0; c.a2 = a2 / a0;
    return c;
}

// Stereo cascade of the 5 bands. Not thread-safe: the audio thread owns one
// and calls configure() when the settings change.
class Equalizer {
public:
    void configure(const Gains& g, double rate) {
        static constexpr double kFreq[kBands] = {80, 400, 1000, 3500, 10000};
        static constexpr Shape kShape[kBands] = {Shape::LowShelf, Shape::Peak, Shape::Peak,
                                                 Shape::Peak, Shape::HighShelf};
        flat_ = true;
        for (int b = 0; b < kBands; ++b) {
            coef_[b] = design(kShape[b], kFreq[b], g[b], rate);
            flat_ = flat_ && g[b] == 0;
        }
        state_ = {};
    }
    [[nodiscard]] bool flat() const { return flat_; }

    // In place, interleaved L/R. Direct form I per band and channel.
    void process(int32_t* lr, size_t frames) {
        if (flat_) return;
        for (size_t f = 0; f < frames; ++f) {
            for (int ch = 0; ch < 2; ++ch) {
                double x = lr[f * 2 + ch];
                for (int b = 0; b < kBands; ++b) {
                    auto& s = state_[b][ch];
                    const auto& c = coef_[b];
                    double y = c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2 - c.a1 * s.y1 - c.a2 * s.y2;
                    s.x2 = s.x1; s.x1 = x; s.y2 = s.y1; s.y1 = y;
                    x = y;
                }
                lr[f * 2 + ch] = static_cast<int32_t>(std::lround(x));
            }
        }
    }

private:
    struct State { double x1 = 0, x2 = 0, y1 = 0, y2 = 0; };
    std::array<Biquad, kBands> coef_{};
    std::array<std::array<State, 2>, kBands> state_{};
    bool flat_ = true;
};

}  // namespace fbx::xserver::eq
