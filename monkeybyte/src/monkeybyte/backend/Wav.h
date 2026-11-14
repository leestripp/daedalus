#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace monkeybyte {

struct WavAudio {
    std::uint32_t sample_rate = 0;
    std::vector<float> samples;
};

// Capture helper: mono floats [-1,1] -> 16 kHz, 16-bit PCM WAV.
std::vector<std::uint8_t> make_wav_16k_mono(std::span<const float> samples);

// Parse a mono integer-PCM WAV (16/24/32-bit) for PipeWire playback.
// Returns false with a human-readable error on malformed/unsupported input.
bool parse_wav(std::span<const std::uint8_t> bytes, WavAudio& out,
               std::string& error);

// OpenAI-compatible multipart body with file + model parts. The returned
// string is a byte container and may include NULs; do not treat it as text.
std::string make_stt_multipart(std::string_view boundary, std::string_view model,
                               std::span<const std::uint8_t> wav);

} // namespace monkeybyte
