#include "monkeybyte/backend/AudioEngine.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace monkeybyte {

void normalise_playback_samples(std::vector<float>& samples) {
    if (samples.empty()) return;
    double sum_squares = 0.0;
    double peak = 0.0;
    for (auto& sample : samples) {
        if (!std::isfinite(sample)) sample = 0.0f;
        const double value = sample;
        sum_squares += value * value;
        peak = std::max(peak, std::abs(value));
    }
    const double rms = std::sqrt(sum_squares / samples.size());
    if (rms < 1e-9 || peak < 1e-9) return;

    constexpr double target_rms = 0.12;
    constexpr double max_gain = 4.0;
    constexpr double peak_limit = 0.98;
    const double gain = std::min({max_gain, target_rms / rms, peak_limit / peak});
    for (auto& sample : samples)
        sample = static_cast<float>(sample * gain);
}

namespace {

class Capture final {
public:
    ~Capture() { (void)stop(); }

    void start() {
        if (loop_) return;
        samples_.clear();
        try {
            loop_ = pw_thread_loop_new("monkeybyte-capture", nullptr);
            if (!loop_) throw std::runtime_error("PipeWire capture loop creation failed");

            // PipeWire retains this pointer for the stream's lifetime.
            // A stack-local events table caused a realtime-thread segfault.
            static const pw_stream_events events = [] {
                pw_stream_events value{};
                value.version = PW_VERSION_STREAM_EVENTS;
                value.process = &Capture::on_process;
                return value;
            }();
            auto* props = pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                PW_KEY_MEDIA_ROLE, "Communication", nullptr);
            if (!props) throw std::runtime_error("PipeWire capture properties failed");
            stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_),
                                           "monkeybyte-microphone", props,
                                           &events, this);
            if (!stream_) throw std::runtime_error("PipeWire capture stream creation failed");

            std::uint8_t buffer[1024];
            spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
            spa_audio_info_raw info = {};
            info.format = SPA_AUDIO_FORMAT_F32_LE;
            info.rate = 16000;
            info.channels = 1;
            info.position[0] = SPA_AUDIO_CHANNEL_MONO;
            const spa_pod* param =
                spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
            const spa_pod* params[] = {param};
            if (pw_stream_connect(stream_, PW_DIRECTION_INPUT, PW_ID_ANY,
                    static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                    PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
                    params, 1) < 0) {
                throw std::runtime_error("PipeWire microphone connection failed");
            }
            if (pw_thread_loop_start(loop_) < 0)
                throw std::runtime_error("PipeWire capture loop start failed");
            loop_started_ = true;
            active_.store(true, std::memory_order_release);
        } catch (...) {
            (void)stop();
            throw;
        }
    }

    std::vector<float> stop() {
        active_.store(false, std::memory_order_release);
        if (loop_) {
            if (loop_started_) {
                pw_thread_loop_lock(loop_);
                if (stream_) {
                    pw_stream_destroy(stream_);
                    stream_ = nullptr;
                }
                // Never hold the loop lock across stop(): stop joins the
                // loop thread (ChimpEcho's proven teardown order).
                pw_thread_loop_unlock(loop_);
                pw_thread_loop_stop(loop_);
            } else if (stream_) {
                pw_stream_destroy(stream_);
                stream_ = nullptr;
            }
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
            loop_started_ = false;
        }
        std::lock_guard lock(mutex_);
        auto out = std::move(samples_);
        samples_.clear();
        return out;
    }

private:
    static void on_process(void* userdata) {
        auto& self = *static_cast<Capture*>(userdata);
        while (auto* buffer = pw_stream_dequeue_buffer(self.stream_)) {
            spa_data& data = buffer->buffer->datas[0];
            if (self.active_.load(std::memory_order_acquire) && data.data && data.chunk) {
                const std::size_t offset = data.chunk->offset;
                const std::size_t size = data.chunk->size;
                if (offset <= data.maxsize && size <= data.maxsize - offset) {
                    const auto* bytes = static_cast<const std::uint8_t*>(data.data);
                    const std::size_t byte_count = size - (size % sizeof(float));
                    const std::size_t count = byte_count / sizeof(float);
                    std::lock_guard lock(self.mutex_);
                    const std::size_t old_size = self.samples_.size();
                    self.samples_.resize(old_size + count);
                    // PipeWire delivers packed bytes; memcpy avoids float
                    // pointer alignment/aliasing assumptions on every arch.
                    std::memcpy(self.samples_.data() + old_size,
                                bytes + offset, byte_count);
                }
            }
            pw_stream_queue_buffer(self.stream_, buffer);
        }
    }

    pw_thread_loop* loop_ = nullptr;
    pw_stream* stream_ = nullptr;
    bool loop_started_ = false;
    std::atomic<bool> active_{false};
    std::mutex mutex_;
    std::vector<float> samples_;
};

class Playback final {
public:
    ~Playback() { stop(); }

    void set_volume(float volume) {
        volume_.store(std::clamp(volume, 0.0F, 1.0F), std::memory_order_release);
    }

    void start(std::uint32_t rate, std::vector<float> samples,
               std::function<void()> on_drained) {
        if (samples.empty()) throw std::invalid_argument("Cannot play empty audio");
        // TTS voices have different loudness. Normalise every utterance
        // before it reaches the stream, including Settings previews.
        normalise_playback_samples(samples);
        stop();
        {
            std::lock_guard lock(mutex_);
            samples_ = std::move(samples);
            position_ = 0;
            flush_requested_ = false;
            on_drained_ = std::move(on_drained);
        }
        drained_pending_.store(false, std::memory_order_release);

        try {
            loop_ = pw_thread_loop_new("monkeybyte-playback", nullptr);
            if (!loop_) throw std::runtime_error("PipeWire playback loop creation failed");
            static const pw_stream_events events = [] {
                pw_stream_events value{};
                value.version = PW_VERSION_STREAM_EVENTS;
                value.process = &Playback::on_process;
                value.drained = &Playback::on_drained;
                return value;
            }();
            auto* props = pw_properties_new(
                PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
                PW_KEY_MEDIA_ROLE, "Communication", nullptr);
            if (!props) throw std::runtime_error("PipeWire playback properties failed");
            stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_),
                                           "monkeybyte-speech", props,
                                           &events, this);
            if (!stream_) throw std::runtime_error("PipeWire playback stream creation failed");

            std::uint8_t buffer[1024];
            spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
            spa_audio_info_raw info = {};
            info.format = SPA_AUDIO_FORMAT_F32_LE;
            info.rate = rate;
            info.channels = 1;
            info.position[0] = SPA_AUDIO_CHANNEL_MONO;
            const spa_pod* param =
                spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
            const spa_pod* params[] = {param};
            if (pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                    static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                    PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
                    params, 1) < 0) {
                throw std::runtime_error("PipeWire speaker connection failed");
            }
            if (pw_thread_loop_start(loop_) < 0)
                throw std::runtime_error("PipeWire playback loop start failed");
            loop_started_ = true;
        } catch (...) {
            stop();
            throw;
        }
    }

    void stop() {
        drained_pending_.store(false, std::memory_order_release);
        if (loop_) {
            if (loop_started_) {
                pw_thread_loop_lock(loop_);
                if (stream_) {
                    pw_stream_destroy(stream_);
                    stream_ = nullptr;
                }
                pw_thread_loop_unlock(loop_);
                pw_thread_loop_stop(loop_);
            } else if (stream_) {
                pw_stream_destroy(stream_);
                stream_ = nullptr;
            }
            pw_thread_loop_destroy(loop_);
            loop_ = nullptr;
            loop_started_ = false;
        }
        std::lock_guard lock(mutex_);
        samples_.clear();
        position_ = 0;
        flush_requested_ = false;
        on_drained_ = {};
    }

private:
    static void on_process(void* userdata) {
        auto& self = *static_cast<Playback*>(userdata);
        while (auto* buffer = pw_stream_dequeue_buffer(self.stream_)) {
            spa_data& data = buffer->buffer->datas[0];
            if (data.data && data.chunk) {
                const std::size_t frames = data.maxsize / sizeof(float);
                auto* destination = static_cast<float*>(data.data);
                bool request_drain = false;
                {
                    std::lock_guard lock(self.mutex_);
                    const std::size_t remaining = self.samples_.size() - self.position_;
                    const std::size_t count = std::min(frames, remaining);
                    std::copy_n(self.samples_.begin() +
                                static_cast<std::ptrdiff_t>(self.position_), count,
                                destination);
                    const float volume =
                        self.volume_.load(std::memory_order_acquire);
                    if (volume != 1.0F) {
                        std::for_each_n(destination, count,
                            [volume](float& sample) { sample *= volume; });
                    }
                    self.position_ += count;
                    std::fill(destination + count, destination + frames, 0.0f);
                    data.chunk->size = frames * sizeof(float);
                    if (self.position_ == self.samples_.size() &&
                        !self.flush_requested_) {
                        self.flush_requested_ = true;
                        request_drain = true;
                    }
                }
                if (request_drain) {
                    // Samples are queued, not necessarily heard. Completion
                    // is emitted only from drained, so the tail is not clipped.
                    self.drained_pending_.store(true, std::memory_order_release);
                    pw_stream_flush(self.stream_, true);
                }
            }
            pw_stream_queue_buffer(self.stream_, buffer);
        }
    }

    static void on_drained(void* userdata) {
        auto& self = *static_cast<Playback*>(userdata);
        if (!self.drained_pending_.exchange(false, std::memory_order_acq_rel)) return;
        std::function<void()> callback;
        {
            std::lock_guard lock(self.mutex_);
            callback = self.on_drained_;
        }
        if (callback) callback();
    }

    pw_thread_loop* loop_ = nullptr;
    pw_stream* stream_ = nullptr;
    bool loop_started_ = false;
    std::mutex mutex_;
    std::vector<float> samples_;
    std::size_t position_ = 0;
    bool flush_requested_ = false;
    std::atomic<bool> drained_pending_{false};
    std::function<void()> on_drained_;
    std::atomic<float> volume_{1.0F};
};

class PipeWireAudio final : public AudioBackend {
public:
    PipeWireAudio() { pw_init(nullptr, nullptr); }
    ~PipeWireAudio() override {
        stop_playback();
        (void)capture_.stop();
        pw_deinit();
    }

    void start_recording() override { capture_.start(); }
    std::vector<float> stop_recording() override { return capture_.stop(); }
    void play(std::uint32_t sample_rate, std::vector<float> samples,
              std::function<void()> on_drained) override {
        playback_.start(sample_rate, std::move(samples), std::move(on_drained));
    }
    void stop_playback() override { playback_.stop(); }
    void set_volume(float volume) override { playback_.set_volume(volume); }

private:
    Capture capture_;
    Playback playback_;
};

} // namespace

std::shared_ptr<AudioBackend> make_pipewire_audio() {
    return std::make_shared<PipeWireAudio>();
}

} // namespace monkeybyte
