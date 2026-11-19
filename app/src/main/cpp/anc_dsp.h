// NoiseCancel – DSP-Kern
//
// Prinzip (aktive Geräuschunterdrückung mit Latenzkompensation):
//
//   Mikrofon -> DC-Blocker -> linearphasiger Tiefpass + Dezimierung (~4 kHz)
//            -> adaptiver NLMS-Prädiktor (sagt das Signal H Abtastwerte voraus)
//            -> Polyphasen-Interpolation zurück auf die Gerätefrequenz
//            -> Feinverzögerung (E Samples) -> Invertierung * Stärke -> Limiter
//
// Ein Handy hat 10–40 ms (Bluetooth 100–300 ms) Audio-Latenz. In dieser Zeit ist
// der Schall, den das Mikrofon gerade hört, längst am Ohr vorbei. Man kann daher
// nicht einfach das Mikrofonsignal invertieren. Stattdessen wird der vorhersagbare
// (periodische/tonale) Anteil des Geräuschs um genau die gemessene Gesamtlatenz
// in die Zukunft vorhergesagt und gegenphasig ausgegeben. Zufälliges Rauschen ist
// nicht vorhersagbar; der Wiener-optimale Prädiktor gibt dafür automatisch ~0 aus
// und erzeugt so kein zusätzliches Rauschen.
//
// Alle Filter sind linearphasig (konstante, exakt bekannte Gruppenlaufzeit), damit
// die Laufzeitkompensation für alle Frequenzen gleichzeitig stimmt.
//
// Diese Datei hat keine Android-Abhängigkeiten und wird auch von der
// Desktop-Simulation (test/sim.cpp) verwendet.

#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace anc {

constexpr double kPi = 3.14159265358979323846;

class AncDsp {
public:
    static constexpr int kPredTaps = 128;          // Prädiktorlänge (32 ms bei 4 kHz)
    static constexpr int kDecHist = 8192;          // Verlauf im dezimierten Bereich (2 s)
    static constexpr int kDecMask = kDecHist - 1;
    static constexpr int kInHist = 4096;           // Verlauf bei Gerätefrequenz
    static constexpr int kInMask = kInHist - 1;
    static constexpr int kELine = 128;             // Feinverzögerungsleitung
    static constexpr int kEMask = kELine - 1;
    static constexpr double kTargetDecRate = 4000.0;
    static constexpr double kCutoffHz = 1300.0;    // Wirksamer Bereich ca. 30–900 Hz
    static constexpr float kOutCeiling = 0.5f;     // max. Ausgangspegel (-6 dBFS), Gehörschutz

    AncDsp() { init(48000); }

    // Nur aufrufen, wenn kein Audio-Thread process() ausführt.
    void init(int sampleRate) {
        fs_ = sampleRate >= 8000 ? sampleRate : 48000;   // 8/16 kHz bei Bluetooth-Freisprechen
        R_ = std::max(1, (int)std::lround(fs_ / kTargetDecRate));
        if (R_ > 90) R_ = 90;
        fsd_ = (double)fs_ / R_;
        nTaps_ = 22 * R_ + 1;                      // ungerade -> ganzzahlige Gruppenlaufzeit
        G_ = (nTaps_ - 1) / 2;
        designFir();

        inHist_.assign(kInHist, 0.f);
        xd_.assign(kDecHist, 0.f);
        U_.assign(kDecHist, 0.f);
        w_.assign(kPredTaps, 0.f);
        eLine_.assign(kELine, 0.f);

        dcR_ = (float)std::exp(-2.0 * kPi * 1.0 / fs_);   // ~1 Hz Hochpass (kaum Phasenfehler bei 50 Hz)
        gSmooth_ = (float)(1.0 - std::exp(-1.0 / (0.05 * fs_)));
        gateSmooth_ = (float)(1.0 - std::exp(-1.0 / (0.01 * fs_)));
        aFast_ = (float)(1.0 - std::exp(-1.0 / (0.05 * fsd_)));
        aSlow_ = (float)(1.0 - std::exp(-1.0 / (8.0 * fsd_)));
        aTrack_ = (float)(1.0 - std::exp(-1.0 / (0.2 * fsd_)));
        aStat_ = (float)(1.0 - std::exp(-1.0 / (1.0 * fsd_)));
        aShort_ = (float)(1.0 - std::exp(-1.0 / (0.08 * fsd_)));
        aMid_ = (float)(1.0 - std::exp(-1.0 / (2.0 * fsd_)));
        aGateDown_ = (float)(1.0 - std::exp(-1.0 / (0.015 * fsd_)));
        aGateUp_ = (float)(1.0 - std::exp(-1.0 / (0.6 * fsd_)));
        warmupSamples_ = (int)(1.0 * fsd_);
        resetState();
    }

    // ---- thread-sichere Parameter (UI/Monitor-Thread) ----
    void setGain(float g) { gainTarget_.store(std::max(0.f, g), std::memory_order_relaxed); }
    void setLoopDelaySeconds(float s) { loopDelay_.store(s, std::memory_order_relaxed); }
    void setOffsetSeconds(float s) { offset_.store(s, std::memory_order_relaxed); }
    void requestReset() { resetReq_.store(true, std::memory_order_relaxed); }
    float loopDelaySeconds() const { return loopDelay_.load(std::memory_order_relaxed); }

    // ---- Statistik (thread-sicher lesbar) ----
    float statInputDb() const { return sInDb_.load(std::memory_order_relaxed); }
    float statOutputDb() const { return sOutDb_.load(std::memory_order_relaxed); }
    float statPredictable() const { return sPred_.load(std::memory_order_relaxed); }
    float statReductionDb() const { return sRedDb_.load(std::memory_order_relaxed); }
    float statHorizonMs() const { return sHorizonMs_.load(std::memory_order_relaxed); }
    float statGainApplied() const { return sGain_.load(std::memory_order_relaxed); }
    float statConfGate() const { return sGate_.load(std::memory_order_relaxed); }
    int statFeedbackEvents() const { return sFeedback_.load(std::memory_order_relaxed); }
    int sampleRate() const { return fs_; }
    int decimation() const { return R_; }
    int groupDelaySamples() const { return G_; }

    // Echtzeit: in[n] = Mikrofon (mono), out[n] = Gegenschall (mono).
    // out[n] wird genau "Gesamtlatenz" nach der Aufnahme von in[n] hörbar.
    void process(const float* in, float* out, int n) {
        for (int i = 0; i < n; ++i) out[i] = step(in[i]);
    }

private:
    void designFir() {
        h_.assign(nTaps_, 0.f);
        const double fc = kCutoffHz / fs_;
        double sum = 0.0;
        std::vector<double> tmp(nTaps_);
        for (int j = 0; j < nTaps_; ++j) {
            const double t = j - G_;
            const double sinc = (t == 0.0) ? 2.0 * fc : std::sin(2.0 * kPi * fc * t) / (kPi * t);
            const double a = 2.0 * kPi * j / (nTaps_ - 1);
            const double win = 0.42 - 0.5 * std::cos(a) + 0.08 * std::cos(2.0 * a);   // Blackman
            tmp[j] = sinc * win;
            sum += tmp[j];
        }
        for (int j = 0; j < nTaps_; ++j) h_[j] = (float)(tmp[j] / sum);
    }

    void resetState() {
        std::fill(inHist_.begin(), inHist_.end(), 0.f);
        std::fill(xd_.begin(), xd_.end(), 0.f);
        std::fill(U_.begin(), U_.end(), 0.f);
        std::fill(w_.begin(), w_.end(), 0.f);
        std::fill(eLine_.begin(), eLine_.end(), 0.f);
        inPos_ = 0; phase_ = 0; m_ = -1; ePos_ = 0;
        dcX1_ = dcY1_ = 0.f;
        gCur_ = 0.f; gate_ = 0.f;
        gTargetLocal_ = gainTarget_.load(std::memory_order_relaxed);
        inFast_ = inSlow_ = outFast_ = 0.f;
        px_ = pe_ = 1e-12f;
        pxS_ = peS_ = 1e-12f; inMid_ = 0.f; ydPow_ = 0.f; conf_ = 0.f; confGate_ = 0.f;
        warm_ = 0; muteCount_ = 0;
        Qcur_ = -1;
        updateHorizon();
    }

    void resetWeights() {
        std::fill(w_.begin(), w_.end(), 0.f);
        px_ = pe_ = 1e-12f;
    }

    // Wandelt (Latenz + Feinabstimmung) in Vorhersagehorizont H (dezimiert)
    // und Restverzögerung E (Gerätefrequenz) um:  H*R - E = Latenz*fs + 2*G + Offset
    void updateHorizon() {
        double q = (double)(loopDelay_.load(std::memory_order_relaxed) +
                            offset_.load(std::memory_order_relaxed)) * fs_ + 2.0 * G_;
        if (q < R_) q = R_;
        const int qi = (int)std::lround(q);
        if (qi == Qcur_) return;
        int H = (qi + R_ - 1) / R_;
        const int maxH = kDecHist - kPredTaps - 64;
        if (H > maxH) H = maxH;
        int E = H * R_ - qi;
        if (E < 0) E = 0;
        if (E >= R_) E = R_ - 1;
        H_ = H; E_ = E; Qcur_ = qi;
        sHorizonMs_.store((float)(1000.0 * (H_ * R_ - E_) / fs_), std::memory_order_relaxed);
    }

    static inline float softLimit(float y) {
        const float t = kOutCeiling * 0.7f;
        const float a = std::fabs(y);
        if (a <= t) return y;
        const float room = kOutCeiling - t;
        const float s = t + room * std::tanh((a - t) / room);
        return y < 0.f ? -s : s;
    }

    inline float step(float in) {
        if (!std::isfinite(in)) in = 0.f;
        // DC-Blocker
        const float x = in - dcX1_ + dcR_ * dcY1_;
        dcX1_ = in; dcY1_ = x;
        inHist_[inPos_] = x;

        // Dezimierung (Phase 0)
        if (phase_ == 0) {
            float acc = 0.f;
            int idx = inPos_;
            for (int j = 0; j < nTaps_; ++j) {
                acc += h_[j] * inHist_[idx];
                idx = (idx - 1) & kInMask;
            }
            onDecimated(acc);
        }

        // Polyphasen-Interpolation
        float v = 0.f;
        int64_t mm = m_;
        for (int j = phase_; j < nTaps_; j += R_) {
            v += h_[j] * U_[(int)(mm & kDecMask)];
            --mm;
        }
        v *= (float)R_;

        // Feinverzögerung
        eLine_[ePos_] = v;
        const float delayed = eLine_[(ePos_ - E_) & kEMask];
        ePos_ = (ePos_ + 1) & kEMask;

        // Stärke (sanft geglättet) + Stummschaltung bei Rückkopplung
        gCur_ += (gTargetLocal_ - gCur_) * gSmooth_;
        gate_ += ((muteCount_ > 0 ? 0.f : 1.f) - gate_) * gateSmooth_;
        float y = -delayed * gCur_ * gate_;
        y = softLimit(y);

        inPos_ = (inPos_ + 1) & kInMask;
        if (++phase_ >= R_) phase_ = 0;
        return y;
    }

    void onDecimated(float z) {
        ++m_;
        xd_[(int)(m_ & kDecMask)] = z;

        if ((m_ & 31) == 0) {
            updateHorizon();
            gTargetLocal_ = gainTarget_.load(std::memory_order_relaxed);
            if (resetReq_.exchange(false, std::memory_order_relaxed)) resetWeights();
        }

        const float z2 = z * z;
        inFast_ += (z2 - inFast_) * aFast_;
        inMid_ += (z2 - inMid_) * aMid_;
        if (warm_ < warmupSamples_) {
            ++warm_;
            inSlow_ = inFast_;
        } else if (muteCount_ > 0) {
            inSlow_ += (inFast_ - inSlow_) * aTrack_;   // stumm -> alles Gehörte ist echt
        } else {
            inSlow_ += (z2 - inSlow_) * aSlow_;
        }

        float yd = 0.f;
        const int H = H_;
        if (m_ >= (int64_t)(H + kPredTaps)) {
            // NLMS-Training: sage xd[m] aus xd[m-H ... m-H-L+1] voraus
            const int64_t base = m_ - H;
            float pred = 0.f, norm = 0.f;
            for (int k = 0; k < kPredTaps; ++k) {
                const float u = xd_[(int)((base - k) & kDecMask)];
                pred += w_[k] * u;
                norm += u * u;
            }
            const float e = z - pred;
            const float g = mu_ * e / (norm + eps_);
            for (int k = 0; k < kPredTaps; ++k) {
                w_[k] = w_[k] * leak_ + g * xd_[(int)((base - k) & kDecMask)];
            }
            px_ += (z2 - px_) * aStat_;
            pe_ += (e * e - pe_) * aStat_;
            pxS_ += (z2 - pxS_) * aShort_;
            peS_ += (e * e - peS_) * aShort_;

            // Vorhersage von xd[m+H]
            float y = 0.f;
            for (int k = 0; k < kPredTaps; ++k) y += w_[k] * xd_[(int)((m_ - k) & kDecMask)];
            if (!std::isfinite(y) || !std::isfinite(pred)) {
                resetWeights();
                y = 0.f;
            }
            // ---- Schutz gegen "lauter statt leiser" ----
            // 1) Kurzzeit-Vorhersagegüte: Gegenschall nur, wenn die Vorhersage von vor
            //    H Samples das JETZIGE Geräusch wirklich getroffen hat.
            conf_ = 1.f - peS_ / std::max(pxS_, 1e-12f);
            float target = (conf_ - kConfLow) / (kConfHigh - kConfLow);
            // 2) Stationarität: ändert sich der Pegel schnell (Auto nähert sich, Tür,
            //    Stimme), ändert sich auch die Schallrichtung -> Phase am Ohr unbekannt.
            const float ratio = (inFast_ + 1e-12f) / (inMid_ + 1e-12f);
            if (ratio > kStatRatio || ratio < 1.f / kStatRatio) target = 0.f;
            target = std::min(1.f, std::max(0.f, target));
            confGate_ += (target - confGate_) * (target < confGate_ ? aGateDown_ : aGateUp_);
            // 3) Der Gegenschall darf nie lauter sein als das Geräusch selbst.
            ydPow_ += (y * y - ydPow_) * aShort_;
            float cap = 1.f;
            if (ydPow_ > inFast_ && ydPow_ > 1e-12f) cap = std::sqrt(inFast_ / ydPow_);
            yd = y * confGate_ * cap;
        }
        U_[(int)(m_ & kDecMask)] = yd;
        outFast_ += (yd * yd * gCur_ * gCur_ * gate_ - outFast_) * aFast_;

        // Rückkopplungsschutz: plötzlicher starker Pegelanstieg bei aktivem Gegenschall
        if (muteCount_ > 0) {
            --muteCount_;
        } else if (warm_ >= warmupSamples_ && gCur_ > 0.02f && inFast_ > 1e-8f &&
                   inFast_ > kFeedbackRatio * inSlow_ && outFast_ > 0.1f * inFast_) {
            sFeedback_.fetch_add(1, std::memory_order_relaxed);
            resetWeights();
            muteCount_ = (int)(1.5 * fsd_);
        }

        if ((m_ & 63) == 0) publishStats();
    }

    void publishStats() {
        const float inDb = 10.f * std::log10(inFast_ + 1e-12f);
        const float outDb = 10.f * std::log10(outFast_ + 1e-12f);
        float pred = std::min(1.f, std::max(0.f, conf_));
        float red = 10.f * std::log10(std::max(px_, 1e-12f) / std::max(pe_, 1e-12f));
        red = std::min(40.f, std::max(0.f, red));
        sInDb_.store(inDb, std::memory_order_relaxed);
        sOutDb_.store(outDb, std::memory_order_relaxed);
        sPred_.store(pred, std::memory_order_relaxed);
        sRedDb_.store(red, std::memory_order_relaxed);
        sGain_.store(gCur_ * gate_, std::memory_order_relaxed);
        sGate_.store(confGate_, std::memory_order_relaxed);
    }

    // Konfiguration
    int fs_ = 48000, R_ = 12, nTaps_ = 265, G_ = 132;
    double fsd_ = 4000.0;
    std::vector<float> h_;
    const float mu_ = 0.01f;
    const float eps_ = 1e-7f;
    const float leak_ = 0.999995f;
    static constexpr float kFeedbackRatio = 30.f;   // ~15 dB Sprung

    // Zustand
    std::vector<float> inHist_, xd_, U_, w_, eLine_;
    int inPos_ = 0, phase_ = 0, ePos_ = 0;
    int64_t m_ = -1;
    int H_ = 1, E_ = 0, Qcur_ = -1;
    float dcR_ = 0.999f, dcX1_ = 0.f, dcY1_ = 0.f;
    float gCur_ = 0.f, gate_ = 0.f, gTargetLocal_ = 0.f, gSmooth_ = 0.001f, gateSmooth_ = 0.002f;
    float aFast_ = 0.005f, aSlow_ = 3e-5f, aTrack_ = 0.001f, aStat_ = 2.5e-4f;
    float inFast_ = 0.f, inSlow_ = 0.f, outFast_ = 0.f, px_ = 1e-12f, pe_ = 1e-12f;
    int warm_ = 0, warmupSamples_ = 4000, muteCount_ = 0;
    float aShort_ = 0.003f, aMid_ = 1.25e-4f, aGateDown_ = 0.016f, aGateUp_ = 4e-4f;
    float pxS_ = 1e-12f, peS_ = 1e-12f, inMid_ = 0.f, ydPow_ = 0.f, conf_ = 0.f, confGate_ = 0.f;
    static constexpr float kConfLow = 0.6f;     // unter 60 % Treffer: kein Gegenschall
    static constexpr float kConfHigh = 0.9f;    // ab 90 %: voller Gegenschall
    static constexpr float kStatRatio = 2.0f;   // > 3 dB Pegeländerung: pausieren

    // Parameter
    std::atomic<float> gainTarget_{1.f};
    std::atomic<float> loopDelay_{0.02f};
    std::atomic<float> offset_{0.f};
    std::atomic<bool> resetReq_{false};

    // Statistik
    std::atomic<float> sInDb_{-120.f}, sOutDb_{-120.f}, sPred_{0.f}, sRedDb_{0.f};
    std::atomic<float> sHorizonMs_{0.f}, sGain_{0.f}, sGate_{0.f};
    std::atomic<int> sFeedback_{0};
};

}  // namespace anc
