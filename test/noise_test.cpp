// Prüft den Rauschgenerator: Pegel, Spitzen, Spektralneigung pro Oktave.
// Optional: ./noise_test out_dir  -> schreibt Hörproben (WAV) rosa.wav / braun.wav
#include "../app/src/main/cpp/noise_gen.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static void writeWav(const std::string& path, const std::vector<float>& lr, int fs) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const uint32_t frames = (uint32_t)(lr.size() / 2), dataBytes = frames * 4;
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(fs); u32(fs * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(dataBytes);
    for (float v : lr) { int s = (int)std::lround(std::fmax(-1.f, std::fmin(1.f, v)) * 32767.f); u16((uint16_t)(int16_t)s); }
    std::fclose(f);
}

// Energie pro Oktavband per einfachem Bandpass (Goertzel-Mittel über Bänder)
static double bandPower(const std::vector<float>& x, int fs, double f1, double f2) {
    const int N = 1 << 15;
    double acc = 0; int cnt = 0;
    for (double f = f1; f < f2; f += (f2 - f1) / 24.0) {
        const double om = 2 * 3.141592653589793 * f / fs, c = 2 * std::cos(om);
        double s1 = 0, s2 = 0;
        for (int i = 0; i < N; ++i) {
            const double hann = 0.5 - 0.5 * std::cos(2 * 3.141592653589793 * i / (N - 1));
            const double s0 = x[2 * i] * hann + c * s1 - s2; s2 = s1; s1 = s0;
        }
        acc += s1 * s1 + s2 * s2 - c * s1 * s2; ++cnt;
    }
    return acc / cnt * (f2 - f1);   // Dichte * Bandbreite = Bandenergie
}

int main(int argc, char** argv) {
    int fail = 0;
    for (int fs : {48000, 16000}) {
        for (int color : {0, 1}) {
            anc::NoiseGen g;
            g.init(fs);
            g.setParams(true, 0.1f, color);
            std::vector<float> lr((size_t)fs * 12 * 2);
            for (size_t i = 0; i < lr.size() / 2; ++i) g.frame(&lr[2 * i], 2);
            // ab Sekunde 4 (nach dem Einblenden) messen
            double acc = 0, peak = 0, corr = 0, l2 = 0, r2 = 0;
            for (size_t i = (size_t)fs * 4; i < lr.size() / 2; ++i) {
                const double l = lr[2 * i], r = lr[2 * i + 1];
                acc += l * l; peak = std::fmax(peak, std::fabs(l)); corr += l * r; l2 += l * l; r2 += r * r;
            }
            const double n = (double)(lr.size() / 2 - (size_t)fs * 4);
            const double rmsDb = 10 * std::log10(acc / n);
            std::vector<float> seg(lr.begin() + (size_t)fs * 4 * 2, lr.end());
            const double p1 = bandPower(seg, fs, 125, 250), p2 = bandPower(seg, fs, 250, 500),
                         p3 = bandPower(seg, fs, 500, 1000), p4 = bandPower(seg, fs, 1000, 2000);
            const double slope = (10 * std::log10(p4 / p1)) / 3.0;   // dB pro Oktave
            const double expected = color == 0 ? 0.0 : -3.0;        // Bandenergie: rosa flach, braun -3 dB/Okt
            const double rho = corr / std::sqrt(l2 * r2);
            const bool ok = std::fabs(rmsDb + 20.0) < 1.0 && peak < 0.6 && std::fabs(slope - expected) < 1.5 && std::fabs(rho) < 0.05;
            std::printf("%s %5d Hz: RMS %.1f dBFS (soll -20), Spitze %.2f, Neigung %+.1f dB/Okt-Band (soll %+.0f), L/R-Korrelation %.3f %s\n",
                        color == 0 ? "Rosa " : "Braun", fs, rmsDb, peak, slope, expected, rho, ok ? "OK" : "FEHLER");
            (void)p2; (void)p3;
            if (!ok) fail = 1;
            if (argc > 1 && fs == 48000) {
                std::vector<float> clip(lr.begin() + (size_t)fs * 2 * 2, lr.begin() + (size_t)fs * 12 * 2);
                writeWav(std::string(argv[1]) + (color == 0 ? "/rosa.wav" : "/braun.wav"), clip, fs);
            }
        }
    }
    return fail;
}
