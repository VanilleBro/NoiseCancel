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
    int fs = 48000;      // Abtastrate
};

static double run(const Case& c, double* inDb, double* outDb, float* pred) {
    const int fs = c.fs;
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

// Abstimm-Assistent: Ohr hört den Schall 1,7 ms später und 40 % leiser als das
// Mikrofon (Handy-Mikro nicht am Ohr). Ein "idealer Hörer" wählt in jeder Stufe
// die leiseste Einstellung – so wie der Nutzer per Tipp.
struct CalibSim {
    int fs = 48000, D;
    anc::AncDsp dsp;
    std::mt19937 rng{3};
    std::normal_distribution<double> nd{0.0, 1.0};
    double ph[4] = {0, 1, 2, 3};
    std::vector<float> hist;          // Mikrofonverlauf für Ohr-Verzögerung
    std::vector<float> outHist;       // Gegenschall-Verlauf (Latenz D)
    long long n = 0;
    const double earDelay = 0.0017, earGain = 0.6;
    CalibSim() : D(25 * 48) {
        dsp.init(fs);
        dsp.setGain(1.0f);
        dsp.setLoopDelaySeconds(0.025f);
        hist.assign(1 << 16, 0.f);
        outHist.assign(1 << 16, 0.f);
    }
    // läuft 'sec' Sekunden, liefert Pegel am Ohr relativ zu ohne ANC (letzte Sekunde)
    double run(double sec) {
        const double f[] = {100, 200, 300, 150};
        const double a[] = {0.05, 0.02, 0.012, 0.01};
        const int B = 192, total = (int)(sec * fs);
        std::vector<float> mic(B), out(B);
        double pa = 0, pr = 0;
        const int eD = (int)std::lround(earDelay * fs);
        for (int k = 0; k < total; k += B) {
            for (int i = 0; i < B; ++i) {
                double v = 0;
                for (int j = 0; j < 4; ++j) { ph[j] += 2 * kPi * f[j] / fs; v += a[j] * std::sin(ph[j]); }
                v += 0.002 * nd(rng);
                mic[i] = (float)v;
                hist[(n + i) & 0xFFFF] = (float)v;
            }
            dsp.process(mic.data(), out.data(), B);
            for (int i = 0; i < B; ++i) {
                const long long t = n + i;
                outHist[t & 0xFFFF] = out[i];
                const double ear = earGain * hist[(t - eD) & 0xFFFF];
                const double r = ear + outHist[(t - D) & 0xFFFF];
                if (k >= total - fs) { pa += ear * ear; pr += r * r; }
            }
            n += B;
        }
        return 10 * std::log10(pr / pa);
    }
};

static void runCalibration() {
    CalibSim cs;
    cs.run(15);
    const double before = cs.run(2);
    const float f0 = cs.dsp.dominantHz();
    std::printf("Assistent: erkannter Brummton %.0f Hz (erwartet 100)\n", f0);
    if (f0 <= 0) return;
    const double T = 1000.0 / f0;            // ms
    // Stufe 1: grob, 16 Schritte über eine Periode
    double bestOff = 0, best = 1e9;
    for (int k = 0; k < 16; ++k) {
        double off = -T / 2 + k * T / 16;
        cs.dsp.setOffsetSeconds((float)(off / 1000));
        const double r = cs.run(1.5);
        if (r < best) { best = r; bestOff = off; }
    }
    // Stufe 2: fein, 5 Schritte um den besten Wert
    const double step = T / 16 / 2.5;
    double fineBest = bestOff;
    for (int k = -2; k <= 2; ++k) {
        const double off = bestOff + k * step;
        cs.dsp.setOffsetSeconds((float)(off / 1000));
        const double r = cs.run(1.5);
        if (r < best) { best = r; fineBest = off; }
    }
    cs.dsp.setOffsetSeconds((float)(fineBest / 1000));
    // Stufe 3: Stärke in 2-dB-Schritten (-6 .. +6 dB)
    double bestG = 1.0;
    for (int k = -3; k <= 3; ++k) {
        const double g = std::pow(10.0, 2.0 * k / 20.0);
        cs.dsp.setGain((float)g);
        const double r = cs.run(1.5);
        if (r < best) { best = r; bestG = g; }
    }
    cs.dsp.setGain((float)bestG);
    const double after = cs.run(3);
    std::printf("Assistent: vorher %+.1f dB -> nachher %+.1f dB (Phase %+.2f ms, Staerke x%.2f)\n",
                before, after, fineBest, bestG);
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
        {"BT-Headset-Mikro (16 kHz), 60 ms", 60, 0, 0, true, 0.002, 0, 16000},
        {"BT-Headset-Mikro (8 kHz), 60 ms", 60, 0, 0, true, 0.002, 0, 8000},
    };
    for (const auto& c : cases) {
        double inDb, outDb; float pred;
        const double d = run(c, &inDb, &outDb, &pred);
        std::printf("%-42s -> Pegel am Ohr: %+6.1f dB   (vorhersagbar %3.0f %%)\n", c.name, d, pred * 100);
    }
    runCalibration();
    runCar(25, 0.0, "Auto, 25 ms, Mikrofon am Ohr");
    runCar(180, 0.0, "Auto, 180 ms (BT), Mikrofon am Ohr");
    runCar(25, 0.4, "Auto, 25 ms, Kabel-Mikro (15 cm)");
    runCar(25, 2.9, "Auto, 25 ms, Handy in Tasche (1 m)");
}
