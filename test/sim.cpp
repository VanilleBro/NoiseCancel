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

// Vorbeifahrendes Auto: Doppler-verschobenes Brummen + tieffrequentes Rollgeräusch,
// Pegel steigt in 4 s um ~25 dB an und fällt wieder. Gemessen wird, ob die App das
// Geräusch irgendwo LAUTER macht (schlimmstes 200-ms-Fenster) und die Gesamtwirkung.
static void runCar(double delayMs, double pathMs, const char* name) {
    const int fs = 48000;
    const int D = (int)std::lround(delayMs * fs / 1000.0);
    anc::AncDsp dsp;
    dsp.init(fs);
    dsp.setGain(1.0f);
    dsp.setLoopDelaySeconds((float)(delayMs / 1000.0));
    std::mt19937 rng(7);
    std::normal_distribution<double> nd(0.0, 1.0);
    const int total = fs * 40;
    std::vector<float> s(total), o(total, 0.f);
    std::vector<float> tau(total, 0.f);   // Laufzeitunterschied Mikrofon->Ohr (s)
    double ph1 = 0, ph2 = 0, rum = 0, rum2 = 0;
    const double carT[] = {12.0, 24.0, 33.0};
    for (int n = 0; n < total; ++n) {
        const double t = (double)n / fs;
        double v = 0.01 * std::sin(2 * kPi * 100.0 * t);           // leiser Lüfter im Hintergrund
        double bestX = 1e9;
        for (double tc : carT) {
            const double x = (t - tc) * 15.0;                        // 15 m/s, 4 m Abstand
            const double dist = std::sqrt(x * x + 16.0);
            const double env = 4.0 / dist;
            const double cosA = -x / dist;
            const double dop = 343.0 / (343.0 - 15.0 * cosA);
            if (std::fabs(x) < std::fabs(bestX)) { bestX = x; tau[n] = (float)(pathMs / 1000.0 * cosA); }
            ph1 += 2 * kPi * 85.0 * dop / fs;
            ph2 += 2 * kPi * 170.0 * dop / fs;
            rum = 0.995 * rum + 0.005 * nd(rng);
            rum2 = 0.99 * rum2 + 0.01 * rum;
            v += env * (0.06 * std::sin(ph1) + 0.03 * std::sin(ph2) + 1.5 * rum2);
        }
        v += 0.001 * nd(rng);
        s[n] = (float)v;
    }
    const int B = 192;
    std::vector<float> mic(B), out(B);
    for (int n0 = 0; n0 + B <= total; n0 += B) {
        for (int i = 0; i < B; ++i) mic[i] = s[n0 + i];
        dsp.process(mic.data(), out.data(), B);
        for (int i = 0; i < B; ++i) o[n0 + i] = out[i];
    }
    const int W = fs / 5;
    double worst = -100, ps = 0, pr = 0;
    for (int n0 = fs * 8; n0 + W <= total; n0 += W) {
        double a = 0, b = 0;
        for (int n = n0; n < n0 + W; ++n) {
            // Signal am Ohr = Mikrofonsignal mit zeitabhängigem Laufzeitunterschied
            const double pos = n - tau[n] * fs;
            const int i0 = (int)std::floor(pos);
            const double fr = pos - i0;
            const double ear = (i0 >= 0 && i0 + 1 < total) ? (1 - fr) * s[i0] + fr * s[i0 + 1] : s[n];
            const double r = ear + (n - D >= 0 ? o[n - D] : 0.f);
            a += ear * ear; b += r * r;
        }
        ps += a; pr += b;
        worst = std::max(worst, 10 * std::log10(b / a));
    }
    std::printf("%-42s -> gesamt %+6.1f dB, schlimmster Moment %+6.1f dB\n",
                name, 10 * std::log10(pr / ps), worst);
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
    runCar(25, 0.0, "Auto, 25 ms, Mikrofon am Ohr");
    runCar(180, 0.0, "Auto, 180 ms (BT), Mikrofon am Ohr");
    runCar(25, 0.4, "Auto, 25 ms, Kabel-Mikro (15 cm)");
    runCar(25, 2.9, "Auto, 25 ms, Handy in Tasche (1 m)");
}
