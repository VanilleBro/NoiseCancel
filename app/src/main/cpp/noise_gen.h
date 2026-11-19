// NoiseCancel – Rauschgenerator zur Geräuschmaskierung
//
// Rosa Rauschen: gleiche Energie pro Oktave (-3 dB/Oktave), klingt wie gleichmäßiger
//   Regen / Wasserfall. Überdeckt Stimmen und mittlere Frequenzen gut.
// Braunes Rauschen: -6 dB/Oktave, tiefes Rauschen wie Brandung / Flugzeugkabine.
//   Überdeckt tiefes Brummen und Verkehr besonders gut.
//
// Beide Kanäle werden unabhängig erzeugt (dekorreliert) -> räumlicher, angenehmer.
// Pegel = Effektivwert (RMS) in dBFS, hart begrenzt für Gehörschutz.

#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

namespace anc {

class NoiseGen {
public:
    static constexpr int kMaxCh = 2;
    static constexpr float kMaxRms = 0.125f;   // -18 dBFS RMS (Spitzen ~ -6 dBFS)

    enum Color { kPink = 0, kBrown = 1 };

    // Nur aufrufen, wenn der Audio-Thread nicht läuft.
    void init(int sampleRate) {
        fs_ = sampleRate > 0 ? sampleRate : 48000;
        const double kTwoPi = 6.283185307179586;
        brownA_ = (float)std::exp(-kTwoPi * 40.0 / fs_);   // Tiefpass 40 Hz -> 1/f² darüber
        hpR_ = (float)std::exp(-kTwoPi * 20.0 / fs_);      // Hochpass 20 Hz (Treiberschutz)
        smooth_ = (float)(1.0 - std::exp(-1.0 / (0.4 * fs_)));
        for (int c = 0; c < kMaxCh; ++c) {
            st_[c] = State();
            st_[c].rng = 0x9E3779B97F4A7C15ULL * (uint64_t)(c + 1) + 12345u;
        }
        // Lautheit beider Farben auf RMS = 1 normieren (abhängig von der Abtastrate)
        pinkNorm_ = 1.f / measureRms(kPink);
        brownNorm_ = 1.f / measureRms(kBrown);
        for (int c = 0; c < kMaxCh; ++c) {
            const uint64_t r = st_[c].rng;
            st_[c] = State();
            st_[c].rng = r;
        }
        levelCur_ = 0.f;
    }

    void setParams(bool on, float rmsLinear, int color) {
        on_.store(on, std::memory_order_relaxed);
        level_.store(std::fmin(kMaxRms, std::fmax(0.f, rmsLinear)), std::memory_order_relaxed);
        color_.store(color == kBrown ? kBrown : kPink, std::memory_order_relaxed);
    }

    // Echtzeit: einen Frame für 'ch' Kanäle erzeugen.
    inline void frame(float* out, int ch) {
        const float target = on_.load(std::memory_order_relaxed) ? level_.load(std::memory_order_relaxed) : 0.f;
        levelCur_ += (target - levelCur_) * smooth_;   // sanftes Ein-/Ausblenden
        const int color = color_.load(std::memory_order_relaxed);
        const float norm = color == kBrown ? brownNorm_ : pinkNorm_;
        for (int c = 0; c < ch; ++c) {
            if (c >= kMaxCh) { out[c] = out[kMaxCh - 1]; continue; }
            out[c] = levelCur_ > 1e-6f ? sample(st_[c], color) * norm * levelCur_ : 0.f;
        }
    }

private:
    struct State {
        uint64_t rng = 1;
        float b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
        float brown = 0, hpX = 0, hpY = 0;
    };

    static inline float white(State& s) {
        // xorshift64* -> zwei gleichverteilte Werte -> Dreiecksverteilung (RMS ~0,41)
        s.rng ^= s.rng >> 12; s.rng ^= s.rng << 25; s.rng ^= s.rng >> 27;
        const uint64_t r = s.rng * 0x2545F4914F6CDD1DULL;
        const float a = (float)(uint32_t)(r >> 32) * (1.f / 4294967296.f);
        const float b = (float)(uint32_t)(r & 0xFFFFFFFFu) * (1.f / 4294967296.f);
        return a + b - 1.f;
    }

    inline float sample(State& s, int color) {
        const float w = white(s);
        float v;
        if (color == kBrown) {
            s.brown = brownA_ * s.brown + (1.f - brownA_) * w;
            v = s.brown;
        } else {
            // Paul Kellets Rosa-Filter
            s.b0 = 0.99886f * s.b0 + w * 0.0555179f;
            s.b1 = 0.99332f * s.b1 + w * 0.0750759f;
            s.b2 = 0.96900f * s.b2 + w * 0.1538520f;
            s.b3 = 0.86650f * s.b3 + w * 0.3104856f;
            s.b4 = 0.55000f * s.b4 + w * 0.5329522f;
            s.b5 = -0.7616f * s.b5 - w * 0.0168980f;
            v = s.b0 + s.b1 + s.b2 + s.b3 + s.b4 + s.b5 + s.b6 + w * 0.5362f;
            s.b6 = w * 0.115926f;
        }
        // Hochpass 20 Hz
        const float y = v - s.hpX + hpR_ * s.hpY;
        s.hpX = v; s.hpY = y;
        return y;
    }

    float measureRms(int color) {
        State s = st_[0];
        const int warm = fs_, n = fs_ * 4;
        double acc = 0;
        for (int i = 0; i < warm + n; ++i) {
            const float v = sample(s, color);
            if (i >= warm) acc += (double)v * v;
        }
        const double rms = std::sqrt(acc / n);
        return rms > 1e-9 ? (float)rms : 1.f;
    }

    int fs_ = 48000;
    State st_[kMaxCh];
    float brownA_ = 0.995f, hpR_ = 0.997f, smooth_ = 5e-5f;
    float pinkNorm_ = 1.f, brownNorm_ = 1.f, levelCur_ = 0.f;
    std::atomic<bool> on_{false};
    std::atomic<float> level_{0.f};
    std::atomic<int> color_{kPink};
};

}  // namespace anc
