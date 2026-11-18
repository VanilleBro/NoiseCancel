// Desktop-Simulation des DSP-Kerns: prüft, ob der Gegenschall nach der
// akustischen Gesamtlatenz das Geräusch am Ohr tatsächlich auslöscht.
// Build: clang++ -O2 -std=c++17 test/sim.cpp -o sim && ./sim
#include "../app/src/main/cpp/anc_dsp.h"
#include <cstdio>
#include <random>
using anc::kPi;

struct Case {
    const char* name;
    double delayMs;      // tatsächliche Gesamtlatenz
    double estErrMs;     // Fehler der Latenzmessung
    double leak;         // Übersprechen Kopfhörer -> Mikrofon
    bool tones;
    double noiseAmp;
    double drift;        // langsame Frequenzdrift (Hz)
};

static double run(const Case& c, double* inDb, double* outDb, float* pred) {
    const int fs = 48000;
    const int D = (int)std::lround(c.delayMs * fs / 1000.0);
    anc::AncDsp dsp;
    dsp.init(fs);
    dsp.setGain(1.0f);
    dsp.setLoopDelaySeconds((float)((c.delayMs + c.estErrMs) / 1000.0));

    std::mt19937 rng(1);
    std::normal_distribution<double> nd(0.0, 1.0);
    const int total = fs * 30, measureFrom = fs * 20;
    std::vector<float> s(total), o(total, 0.f);
    const double f[] = {60, 120, 180, 300, 437, 610};
    const double a[] = {0.05, 0.03, 0.02, 0.015, 0.01, 0.006};
    double ph[6] = {0, 1, 2, 3, 4, 5};
    for (int n = 0; n < total; ++n) {
        double v = 0;
        if (c.tones) {
            for (int k = 0; k < 6; ++k) {
                const double fk = f[k] + c.drift * std::sin(2 * kPi * 0.05 * n / fs);
                ph[k] += 2 * kPi * fk / fs;
                v += a[k] * std::sin(ph[k]);
            }
        }
        v += c.noiseAmp * nd(rng);
        s[n] = (float)v;
    }
    // simple 2-pole Tiefpass 1 kHz für die Bewertung im Wirkbereich
    auto lp = [&](const std::vector<float>& x) {
        std::vector<float> y(x.size());
        const double w = 2 * kPi * 1000.0 / fs, al = std::exp(-w);
        double s1 = 0, s2 = 0;
        for (size_t i = 0; i < x.size(); ++i) { s1 = (1 - al) * x[i] + al * s1; s2 = (1 - al) * s1 + al * s2; y[i] = (float)s2; }
        return y;
    };

    const int B = 192;
    std::vector<float> mic(B), out(B);
    for (int n0 = 0; n0 + B <= total; n0 += B) {
        for (int i = 0; i < B; ++i) {
            const int n = n0 + i;
            double m = s[n];
            if (c.leak > 0 && n - D >= 0) m += c.leak * o[n - D];   // Übersprechen
            mic[i] = (float)m;
        }
        dsp.process(mic.data(), out.data(), B);
        for (int i = 0; i < B; ++i) o[n0 + i] = out[i];
    }
    std::vector<float> r(total);
    for (int n = 0; n < total; ++n) r[n] = s[n] + (n - D >= 0 ? o[n - D] : 0.f);
    auto sl = lp(s), rl = lp(r);
    double ps = 0, pr = 0;
    for (int n = measureFrom; n < total; ++n) { ps += sl[n] * sl[n]; pr += rl[n] * rl[n]; }
    *inDb = dsp.statInputDb();
    *outDb = dsp.statOutputDb();
    *pred = dsp.statPredictable();
    return 10 * std::log10(pr / ps);
}

int main() {
    const Case cases[] = {
        {"Toene, 25 ms Latenz (Kabel)", 25, 0, 0, true, 0.002, 0},
        {"Toene, 12 ms Latenz (MMAP)", 12, 0, 0, true, 0.002, 0},
        {"Toene, 180 ms Latenz (Bluetooth)", 180, 0, 0, true, 0.002, 0},
        {"Toene + starkes Rauschen, 25 ms", 25, 0, 0, true, 0.02, 0},
        {"Toene, 25 ms, 0.3 ms Messfehler", 25, 0.3, 0, true, 0.002, 0},
        {"Toene, 25 ms, Uebersprechen 20 %", 25, 0, 0.2, true, 0.002, 0},
        {"Toene, 25 ms, Frequenzdrift 0.5 Hz", 25, 0, 0, true, 0.002, 0.5},
        {"Nur Rauschen (darf nicht lauter werden)", 25, 0, 0, false, 0.02, 0},
        {"Lautsprecher, Rueckkopplung 150 %", 25, 0, 1.5, true, 0.002, 0},
    };
    for (const auto& c : cases) {
        double inDb, outDb; float pred;
        const double d = run(c, &inDb, &outDb, &pred);
        std::printf("%-42s -> Pegel am Ohr: %+6.1f dB   (vorhersagbar %3.0f %%)\n", c.name, d, pred * 100);
    }
}
