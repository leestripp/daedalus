#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace monkeybyte {

// §CHIMPFLOW — substitutable audio boundary. Production uses PipeWire; unit
// tests use a fake, so no microphone/speaker is needed for orchestration.
class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    virtual void start_recording() = 0;
    virtual std::vector<float> stop_recording() = 0;
    virtual void play(std::uint32_t sample_rate, std::vector<float> samples,
                      std::function<void()> on_drained) = 0;
    virtual void stop_playback() = 0;
    virtual void set_volume(float volume) = 0;
};

// RMS loudness normalisation shared by every PipeWire playback. Targets
// 0.12 RMS, limits gain to 4x, and keeps peaks at or below 0.98. Silence is
// unchanged; non-finite input samples are sanitized to zero.
void normalise_playback_samples(std::vector<float>& samples);

// Construction initializes PipeWire only; no device/stream opens until a
// recording or playback operation is requested.
std::shared_ptr<AudioBackend> make_pipewire_audio();

} // namespace monkeybyte
