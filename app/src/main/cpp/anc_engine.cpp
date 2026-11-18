// NoiseCancel – Echtzeit-Audio-Engine (AAudio, Full-Duplex, niedrige Latenz) + JNI
//
// Ausgabe-Stream mit Datencallback; im Callback wird der Eingabe-Stream nicht
// blockierend gelesen (gleiches Muster wie Oboes FullDuplexStream). Ein
// Monitor-Thread misst die echte akustische Gesamtlatenz Mikrofon->Kopfhörer
// über die Hardware-Zeitstempel beider Streams und gibt sie an den DSP weiter.

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <jni.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "anc_dsp.h"

#define LOG_TAG "NoiseCancel"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr int kMaxFrames = 8192;
constexpr int kDrainCallbacks = 20;
constexpr int64_t kInvalid = INT64_MIN;

void stopAndClose(AAudioStream*& s) {
    if (!s) return;
    AAudioStream_requestStop(s);
    aaudio_stream_state_t st = AAudioStream_getState(s);
    for (int i = 0; i < 10; ++i) {
        if (st == AAUDIO_STREAM_STATE_STOPPED || st == AAUDIO_STREAM_STATE_DISCONNECTED ||
            st == AAUDIO_STREAM_STATE_CLOSED || st == AAUDIO_STREAM_STATE_UNINITIALIZED) break;
        aaudio_stream_state_t next = AAUDIO_STREAM_STATE_UNINITIALIZED;
        if (AAudioStream_waitForStateChange(s, st, &next, 100000000LL) != AAUDIO_OK) break;
        st = next;
    }
    AAudioStream_close(s);
    s = nullptr;
}

class Engine {
public:
    bool start() {
        std::lock_guard<std::mutex> life(lifeMtx_);
        if (running_.load()) return true;
        {
            std::lock_guard<std::mutex> lk(streamMtx_);
            setError("");
            if (!openStreams()) {
                closeStreams();
                return false;
            }
        }
        running_.store(true);
        monitorRun_.store(true);
        monitor_ = std::thread(&Engine::monitorLoop, this);
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> life(lifeMtx_);
        monitorRun_.store(false);
        if (monitor_.joinable()) monitor_.join();
        std::lock_guard<std::mutex> lk(streamMtx_);
        closeStreams();
        running_.store(false);
    }

    void setGain(float g) { dsp_.setGain(g); gain_.store(g); }
    void setOffsetMs(float ms) { dsp_.setOffsetSeconds(ms / 1000.f); offsetMs_.store(ms); }
    void resetFilter() { dsp_.requestReset(); }

    void getStats(float* o, int n) {
        float v[16] = {0};
        v[0] = running_.load() ? 1.f : 0.f;
        v[1] = (float)sampleRate_.load();
        v[2] = latencyMs_.load();
        v[3] = latencyMeasured_.load() ? 1.f : 0.f;
        v[4] = dsp_.statInputDb();
        v[5] = dsp_.statOutputDb();
        v[6] = dsp_.statPredictable();
        v[7] = dsp_.statReductionDb();
        v[8] = (float)dsp_.statFeedbackEvents();
        v[9] = (float)(underruns_.load() + xruns_.load());
        v[10] = exclusive_.load() ? 1.f : 0.f;
        v[11] = dsp_.statHorizonMs();
        v[12] = dsp_.statGainApplied();
        v[13] = restarting_.load() ? 1.f : 0.f;
        v[14] = (float)outBufferFrames_.load();
        v[15] = lowLatency_.load() ? 1.f : 0.f;
        for (int i = 0; i < n && i < 16; ++i) o[i] = v[i];
    }

    std::string lastError() {
        std::lock_guard<std::mutex> lk(errMtx_);
        return lastError_;
    }

private:
    void setError(const std::string& e) {
        std::lock_guard<std::mutex> lk(errMtx_);
        lastError_ = e;
        if (!e.empty()) LOGE("%s", e.c_str());
    }

    AAudioStream* openOne(aaudio_direction_t dir, int rate, int channels, aaudio_input_preset_t preset,
                          bool withCallback, aaudio_result_t* resOut) {
        AAudioStreamBuilder* b = nullptr;
        aaudio_result_t r = AAudio_createStreamBuilder(&b);
        if (r != AAUDIO_OK) { *resOut = r; return nullptr; }
        AAudioStreamBuilder_setDirection(b, dir);
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_EXCLUSIVE);
        AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(b, channels);
        if (rate > 0) AAudioStreamBuilder_setSampleRate(b, rate);
        if (dir == AAUDIO_DIRECTION_INPUT) {
            AAudioStreamBuilder_setInputPreset(b, preset);
        } else {
            AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
            AAudioStreamBuilder_setContentType(b, AAUDIO_CONTENT_TYPE_MUSIC);
        }
        if (withCallback) {
            AAudioStreamBuilder_setDataCallback(b, &Engine::dataCallback, this);
            AAudioStreamBuilder_setErrorCallback(b, &Engine::errorCallback, this);
        }
        AAudioStream* s = nullptr;
        r = AAudioStreamBuilder_openStream(b, &s);
        AAudioStreamBuilder_delete(b);
        *resOut = r;
        if (r != AAUDIO_OK) return nullptr;
        return s;
    }

    // streamMtx_ muss gehalten werden
    bool openStreams() {
        closeStreams();
        aaudio_result_t r = AAUDIO_OK;

        out_ = openOne(AAUDIO_DIRECTION_OUTPUT, 0, 2, AAUDIO_INPUT_PRESET_GENERIC, true, &r);
        if (!out_) {
            setError(std::string("Audioausgabe konnte nicht geöffnet werden: ") + AAudio_convertResultToText(r));
            return false;
        }
        const int rate = AAudioStream_getSampleRate(out_);
        outChannels_ = AAudioStream_getChannelCount(out_);
        outFormat_ = AAudioStream_getFormat(out_);
        const int burst = std::max(16, (int)AAudioStream_getFramesPerBurst(out_));
        outBurst_ = burst;
        AAudioStream_setBufferSizeInFrames(out_, burst * 2);
        outBufferFrames_.store(AAudioStream_getBufferSizeInFrames(out_));

        const aaudio_input_preset_t presets[] = {AAUDIO_INPUT_PRESET_UNPROCESSED,
                                                 AAUDIO_INPUT_PRESET_VOICE_RECOGNITION,
                                                 AAUDIO_INPUT_PRESET_GENERIC};
        for (aaudio_input_preset_t p : presets) {
            in_ = openOne(AAUDIO_DIRECTION_INPUT, rate, 1, p, false, &r);
            if (in_) {
                if (AAudioStream_getSampleRate(in_) == rate) break;
                stopAndClose(in_);
                r = AAUDIO_ERROR_INVALID_RATE;
            }
        }
        if (!in_) {
            setError(std::string("Mikrofon konnte nicht geöffnet werden: ") + AAudio_convertResultToText(r));
            return false;
        }
        inChannels_ = AAudioStream_getChannelCount(in_);
        inFormat_ = AAudioStream_getFormat(in_);
        inBurst_ = std::max(16, (int)AAudioStream_getFramesPerBurst(in_));
        AAudioStream_setBufferSizeInFrames(in_, AAudioStream_getBufferCapacityInFrames(in_));

        if ((outFormat_ != AAUDIO_FORMAT_PCM_FLOAT && outFormat_ != AAUDIO_FORMAT_PCM_I16) ||
            (inFormat_ != AAUDIO_FORMAT_PCM_FLOAT && inFormat_ != AAUDIO_FORMAT_PCM_I16)) {
            setError("Nicht unterstütztes Audioformat");
            return false;
        }

        exclusive_.store(AAudioStream_getSharingMode(out_) == AAUDIO_SHARING_MODE_EXCLUSIVE &&
                         AAudioStream_getSharingMode(in_) == AAUDIO_SHARING_MODE_EXCLUSIVE);
        lowLatency_.store(AAudioStream_getPerformanceMode(out_) == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY &&
                          AAudioStream_getPerformanceMode(in_) == AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        sampleRate_.store(rate);

        inF_.assign((size_t)kMaxFrames * std::max(1, inChannels_), 0.f);
        inI_.assign((size_t)kMaxFrames * std::max(1, inChannels_), 0);
        mono_.assign(kMaxFrames, 0.f);
        anti_.assign(kMaxFrames, 0.f);

        dsp_.init(rate);
        dsp_.setGain(gain_.load());
        dsp_.setOffsetSeconds(offsetMs_.load() / 1000.f);
        // Startschätzung bis zur ersten Messung
        const float guess = (float)(burst * 2 + inBurst_ * 2) / rate + 0.004f;
        dsp_.setLoopDelaySeconds(guess);
        latencyMs_.store(guess * 1000.f);
        latencyMeasured_.store(false);
        latWindow_.clear();

        drainCount_ = 0;
        outFramesTotal_ = 0;
        inFramesTotal_ = 0;
        frameOffset_.store(kInvalid);
        lastXrun_ = 0;
        cbCount_ = 0;
        needRestart_.store(false);

        r = AAudioStream_requestStart(in_);
        if (r != AAUDIO_OK) {
            setError(std::string("Mikrofon-Start fehlgeschlagen: ") + AAudio_convertResultToText(r));
            return false;
        }
        r = AAudioStream_requestStart(out_);
        if (r != AAUDIO_OK) {
            setError(std::string("Ausgabe-Start fehlgeschlagen: ") + AAudio_convertResultToText(r));
            return false;
        }
        LOGI("Streams offen: %d Hz, out burst %d, in burst %d, exklusiv %d, in-ch %d",
             rate, burst, inBurst_, exclusive_.load() ? 1 : 0, inChannels_);
        return true;
    }

    void closeStreams() {
        stopAndClose(out_);   // zuerst Ausgabe -> Callback endet
        stopAndClose(in_);
    }

    static aaudio_data_callback_result_t dataCallback(AAudioStream* s, void* ud, void* data, int32_t n) {
        return static_cast<Engine*>(ud)->onAudio(s, data, n);
    }

    static void errorCallback(AAudioStream*, void* ud, aaudio_result_t err) {
        Engine* e = static_cast<Engine*>(ud);
        LOGE("Stream-Fehler: %s", AAudio_convertResultToText(err));
        e->needRestart_.store(true);   // Neustart erledigt der Monitor-Thread
    }

    int readInput(int frames) {
        int32_t got;
        if (inFormat_ == AAUDIO_FORMAT_PCM_FLOAT) {
            got = AAudioStream_read(in_, inF_.data(), frames, 0);
        } else {
            got = AAudioStream_read(in_, inI_.data(), frames, 0);
        }
        if (got < 0) {
            if (got == AAUDIO_ERROR_DISCONNECTED) needRestart_.store(true);
            return 0;
        }
        return got;
    }

    void inputToMono(int got, int n) {
        const int ch = std::max(1, inChannels_);
        if (inFormat_ == AAUDIO_FORMAT_PCM_FLOAT) {
            for (int i = 0; i < got; ++i) mono_[i] = inF_[(size_t)i * ch];
        } else {
            for (int i = 0; i < got; ++i) mono_[i] = inI_[(size_t)i * ch] * (1.f / 32768.f);
        }
        for (int i = got; i < n; ++i) mono_[i] = 0.f;
    }

    void writeOutput(void* data, int offset, int n, bool silence) {
        const int ch = std::max(1, outChannels_);
        if (outFormat_ == AAUDIO_FORMAT_PCM_FLOAT) {
            float* o = static_cast<float*>(data) + (size_t)offset * ch;
            for (int i = 0; i < n; ++i) {
                const float v = silence ? 0.f : anti_[i];
                for (int c = 0; c < ch; ++c) o[(size_t)i * ch + c] = v;
            }
        } else {
            int16_t* o = static_cast<int16_t*>(data) + (size_t)offset * ch;
            for (int i = 0; i < n; ++i) {
                float v = silence ? 0.f : anti_[i];
                v = std::max(-1.f, std::min(1.f, v));
                const int16_t q = (int16_t)std::lround(v * 32767.f);
                for (int c = 0; c < ch; ++c) o[(size_t)i * ch + c] = q;
            }
        }
    }

    aaudio_data_callback_result_t onAudio(AAudioStream* out, void* data, int32_t numFrames) {
        int done = 0;
        while (done < numFrames) {
            const int n = std::min(kMaxFrames, (int)numFrames - done);
            const int64_t blockStart = outFramesTotal_;

            if (drainCount_ < kDrainCallbacks) {
                // Anfangs Eingangspuffer leeren -> minimale Latenz
                for (int k = 0; k < 64; ++k) {
                    const int got = readInput(kMaxFrames);
                    inFramesTotal_ += got;
                    if (got <= 0) break;
                }
                writeOutput(data, done, n, true);
                outFramesTotal_ += n;
                done += n;
                ++drainCount_;
                continue;
            }

            // Rückstau begrenzen (falls die Eingabe vorausgelaufen ist)
            const int64_t avail = AAudioStream_getFramesWritten(in_) - AAudioStream_getFramesRead(in_);
            const int64_t allowed = (int64_t)n + 2 * inBurst_;
            if (avail > allowed) {
                int64_t drop = avail - n - inBurst_;
                while (drop > 0) {
                    const int got = readInput((int)std::min<int64_t>(drop, kMaxFrames));
                    if (got <= 0) break;
                    inFramesTotal_ += got;
                    drop -= got;
                }
            }

            const int got = readInput(n);
            inFramesTotal_ += got;
            if (got < n) underruns_.fetch_add(1, std::memory_order_relaxed);
            else frameOffset_.store(blockStart - (inFramesTotal_ - got), std::memory_order_relaxed);

            inputToMono(got, n);
            dsp_.process(mono_.data(), anti_.data(), n);
            writeOutput(data, done, n, false);
            outFramesTotal_ += n;
            done += n;
        }

        // Pufferanpassung bei Aussetzern (wie Oboes LatencyTuner)
        if ((++cbCount_ & 63) == 0) {
            const int32_t x = AAudioStream_getXRunCount(out);
            if (x > lastXrun_) {
                xruns_.fetch_add(x - lastXrun_, std::memory_order_relaxed);
                lastXrun_ = x;
                const int32_t cur = AAudioStream_getBufferSizeInFrames(out);
                const int32_t cap = AAudioStream_getBufferCapacityInFrames(out);
                if (cur + outBurst_ <= cap) {
                    AAudioStream_setBufferSizeInFrames(out, cur + outBurst_);
                    outBufferFrames_.store(AAudioStream_getBufferSizeInFrames(out));
                }
            }
        }
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    void measureLatency() {
        std::unique_lock<std::mutex> lk(streamMtx_, std::try_to_lock);
        if (!lk.owns_lock() || !in_ || !out_) return;
        const int64_t off = frameOffset_.load(std::memory_order_relaxed);
        if (off == kInvalid) return;
        int64_t fpi = 0, ti = 0, fpo = 0, to = 0;
        if (AAudioStream_getTimestamp(in_, CLOCK_MONOTONIC, &fpi, &ti) != AAUDIO_OK) return;
        if (AAudioStream_getTimestamp(out_, CLOCK_MONOTONIC, &fpo, &to) != AAUDIO_OK) return;
        const int rate = sampleRate_.load();
        if (rate <= 0) return;
        // Ausgabeframe k wird bei to+(k-fpo)/fs hörbar; der zugehörige Eingabeframe
        // k-off wurde bei ti+(k-off-fpi)/fs aufgenommen.
        const double d = (double)(to - ti) * 1e-9 + (double)(fpi + off - fpo) / rate;
        if (!(d > 0.0005 && d < 1.0)) return;
        latWindow_.push_back(d);
        if (latWindow_.size() > 15) latWindow_.erase(latWindow_.begin());
        std::vector<double> tmp = latWindow_;
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        const double med = tmp[tmp.size() / 2];
        const double cur = dsp_.loopDelaySeconds();
        if (!latencyMeasured_.load() || std::fabs(med - cur) > 0.0003) {
            dsp_.setLoopDelaySeconds((float)med);
            latencyMs_.store((float)(med * 1000.0));
        }
        latencyMeasured_.store(true);
    }

    void monitorLoop() {
        while (monitorRun_.load()) {
            for (int i = 0; i < 10 && monitorRun_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            if (!monitorRun_.load()) break;

            if (needRestart_.load()) {
                restarting_.store(true);
                bool ok;
                {
                    std::lock_guard<std::mutex> lk(streamMtx_);
                    closeStreams();
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    ok = openStreams();
                    if (!ok) closeStreams();
                }
                if (ok) {
                    restarting_.store(false);
                } else {
                    needRestart_.store(true);
                    for (int i = 0; i < 50 && monitorRun_.load(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                continue;
            }
            measureLatency();
        }
    }

    anc::AncDsp dsp_;
    std::mutex lifeMtx_, streamMtx_, errMtx_;
    std::string lastError_;
    AAudioStream* in_ = nullptr;
    AAudioStream* out_ = nullptr;
    std::thread monitor_;
    std::atomic<bool> running_{false}, monitorRun_{false}, needRestart_{false}, restarting_{false};
    std::atomic<bool> exclusive_{false}, lowLatency_{false}, latencyMeasured_{false};
    std::atomic<int> sampleRate_{0}, outBufferFrames_{0};
    std::atomic<float> latencyMs_{0.f}, gain_{1.f}, offsetMs_{0.f};
    std::atomic<int64_t> frameOffset_{kInvalid};
    std::atomic<int> underruns_{0}, xruns_{0};

    int outChannels_ = 2, inChannels_ = 1, outBurst_ = 192, inBurst_ = 192;
    aaudio_format_t outFormat_ = AAUDIO_FORMAT_PCM_FLOAT, inFormat_ = AAUDIO_FORMAT_PCM_FLOAT;
    std::vector<float> inF_, mono_, anti_;
    std::vector<int16_t> inI_;
    std::vector<double> latWindow_;
    int drainCount_ = 0, cbCount_ = 0;
    int32_t lastXrun_ = 0;
    int64_t outFramesTotal_ = 0, inFramesTotal_ = 0;
};

Engine& engine() {
    static Engine e;
    return e;
}

}  // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_vanillebro_noisecancel_NativeEngine_nativeStart(JNIEnv*, jclass) {
    return engine().start() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_com_vanillebro_noisecancel_NativeEngine_nativeStop(JNIEnv*, jclass) {
    engine().stop();
}

JNIEXPORT void JNICALL
Java_com_vanillebro_noisecancel_NativeEngine_nativeSetGain(JNIEnv*, jclass, jfloat g) {
    engine().setGain(g);
}

JNIEXPORT void JNICALL
Java_com_vanillebro_noisecancel_NativeEngine_nativeSetOffsetMs(JNIEnv*, jclass, jfloat ms) {
    engine().setOffsetMs(ms);
}

JNIEXPORT void JNICALL
Java_com_vanillebro_noisecancel_NativeEngine_nativeResetFilter(JNIEnv*, jclass) {
    engine().resetFilter();
}

JNIEXPORT void JNICALL
Java_com_vanillebro_noisecancel_NativeEngine_nativeGetStats(JNIEnv* env, jclass, jfloatArray arr) {
    if (!arr) return;
    const jsize n = env->GetArrayLength(arr);
    float tmp[16] = {0};
    engine().getStats(tmp, 16);
    env->SetFloatArrayRegion(arr, 0, std::min<jsize>(n, 16), tmp);
}

JNIEXPORT jstring JNICALL
Java_com_vanillebro_noisecancel_NativeEngine_nativeGetLastError(JNIEnv* env, jclass) {
    return env->NewStringUTF(engine().lastError().c_str());
}

}  // extern "C"
