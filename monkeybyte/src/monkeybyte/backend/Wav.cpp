#include "monkeybyte/backend/Wav.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace monkeybyte {

namespace {

std::uint16_t read_u16(std::span<const std::uint8_t> bytes, std::size_t pos) {
    return static_cast<std::uint16_t>(bytes[pos]) |
           static_cast<std::uint16_t>(bytes[pos + 1] << 8);
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t pos) {
    return static_cast<std::uint32_t>(bytes[pos]) |
           (static_cast<std::uint32_t>(bytes[pos + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[pos + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[pos + 3]) << 24);
}

void write_u16(std::vector<std::uint8_t>& bytes, std::size_t pos, std::uint16_t value) {
    bytes[pos] = static_cast<std::uint8_t>(value & 0xff);
    bytes[pos + 1] = static_cast<std::uint8_t>(value >> 8);
}

void write_u32(std::vector<std::uint8_t>& bytes, std::size_t pos, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i)
        bytes[pos + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

bool has_id(std::span<const std::uint8_t> bytes, std::size_t pos,
            std::string_view id) {
    return pos + id.size() <= bytes.size() &&
           std::equal(id.begin(), id.end(), bytes.begin() +
                      static_cast<std::ptrdiff_t>(pos));
}

bool fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

} // namespace

std::vector<std::uint8_t> make_wav_16k_mono(std::span<const float> samples) {
    constexpr std::uint32_t rate = 16000;
    constexpr std::size_t header_size = 44;
    if (samples.size() > (std::numeric_limits<std::uint32_t>::max() - 36u) / 2u)
        throw std::length_error("too many samples for a RIFF/WAV file");

    const auto data_size = static_cast<std::uint32_t>(samples.size() * 2);
    std::vector<std::uint8_t> out(header_size + data_size, 0);
    const auto id = [&out](std::size_t pos, std::string_view value) {
        std::copy(value.begin(), value.end(), out.begin() +
                  static_cast<std::ptrdiff_t>(pos));
    };
    id(0, "RIFF");
    write_u32(out, 4, 36u + data_size);
    id(8, "WAVE");
    id(12, "fmt ");
    write_u32(out, 16, 16);
    write_u16(out, 20, 1); // integer PCM
    write_u16(out, 22, 1); // mono
    write_u32(out, 24, rate);
    write_u32(out, 28, rate * 2u);
    write_u16(out, 32, 2);
    write_u16(out, 34, 16);
    id(36, "data");
    write_u32(out, 40, data_size);

    for (std::size_t i = 0; i < samples.size(); ++i) {
        const float clamped = std::clamp(samples[i], -1.0f, 1.0f);
        const auto value = static_cast<std::int16_t>(std::lrint(clamped * 32767.0f));
        write_u16(out, header_size + i * 2, static_cast<std::uint16_t>(value));
    }
    return out;
}

bool parse_wav(std::span<const std::uint8_t> bytes, WavAudio& out,
               std::string& error) {
    out = {};
    error.clear();
    if (bytes.size() < 12 || !has_id(bytes, 0, "RIFF") || !has_id(bytes, 8, "WAVE"))
        return fail(error, "not a RIFF/WAV stream");

    bool have_format = false;
    std::uint16_t channels = 0;
    std::uint16_t bits = 0;
    std::uint16_t block_align = 0;
    std::uint32_t rate = 0;
    std::size_t data_pos = 0;
    std::size_t data_size = 0;

    std::size_t pos = 12;
    while (pos < bytes.size()) {
        if (bytes.size() - pos < 8)
            return fail(error, "truncated WAV chunk header");
        const std::uint32_t chunk_size_u32 = read_u32(bytes, pos + 4);
        const auto chunk_size = static_cast<std::size_t>(chunk_size_u32);
        const std::size_t chunk_pos = pos + 8;
        if (chunk_size > bytes.size() - chunk_pos)
            return fail(error, "truncated WAV chunk");

        if (has_id(bytes, pos, "fmt ")) {
            if (chunk_size < 16)
                return fail(error, "short WAV format chunk");
            const auto format = read_u16(bytes, chunk_pos);
            channels = read_u16(bytes, chunk_pos + 2);
            rate = read_u32(bytes, chunk_pos + 4);
            block_align = read_u16(bytes, chunk_pos + 12);
            bits = read_u16(bytes, chunk_pos + 14);
            if (format != 1)
                return fail(error, "WAV is not integer PCM");
            have_format = true;
        } else if (has_id(bytes, pos, "data") && data_pos == 0) {
            data_pos = chunk_pos;
            data_size = chunk_size;
        }

        pos = chunk_pos + chunk_size;
        if ((chunk_size & 1u) != 0) {
            if (pos == bytes.size())
                return fail(error, "missing WAV odd-chunk padding");
            ++pos;
        }
    }

    if (!have_format)
        return fail(error, "missing WAV format chunk");
    if (channels != 1)
        return fail(error, "only mono WAV is supported");
    if (bits != 16 && bits != 24 && bits != 32)
        return fail(error, "only 16/24/32-bit PCM WAV is supported");
    if (rate < 8000 || rate > 192000)
        return fail(error, "invalid WAV sample rate");
    if (block_align != bits / 8)
        return fail(error, "invalid WAV block alignment");
    if (data_pos == 0)
        return fail(error, "missing WAV data chunk");

    const std::size_t sample_bytes = bits / 8;
    if (data_size % sample_bytes != 0)
        return fail(error, "partial PCM sample in WAV data");

    WavAudio parsed;
    parsed.sample_rate = rate;
    parsed.samples.reserve(data_size / sample_bytes);
    for (std::size_t pos_in_data = 0; pos_in_data < data_size;
         pos_in_data += sample_bytes) {
        const std::size_t p = data_pos + pos_in_data;
        if (bits == 16) {
            const auto raw = read_u16(bytes, p);
            const std::int32_t value = raw <= 32767u
                                           ? static_cast<std::int32_t>(raw)
                                           : static_cast<std::int32_t>(raw) - 65536;
            parsed.samples.push_back(static_cast<float>(value / 32768.0));
        } else if (bits == 24) {
            std::int32_t value = static_cast<std::int32_t>(bytes[p]) |
                                 (static_cast<std::int32_t>(bytes[p + 1]) << 8) |
                                 (static_cast<std::int32_t>(bytes[p + 2]) << 16);
            if ((value & 0x800000) != 0) value -= 0x1000000;
            parsed.samples.push_back(static_cast<float>(value / 8388608.0));
        } else {
            const auto raw = read_u32(bytes, p);
            const std::int64_t value = raw <= 0x7fffffffu
                                           ? static_cast<std::int64_t>(raw)
                                           : static_cast<std::int64_t>(raw) - 0x100000000LL;
            parsed.samples.push_back(static_cast<float>(value / 2147483648.0));
        }
    }
    out = std::move(parsed);
    return true;
}

std::string make_stt_multipart(std::string_view boundary, std::string_view model,
                               std::span<const std::uint8_t> wav) {
    if (boundary.empty() || boundary.find_first_of("\r\n") != std::string_view::npos)
        throw std::invalid_argument("invalid multipart boundary");
    if (model.empty() || model.find_first_of("\r\n") != std::string_view::npos)
        throw std::invalid_argument("invalid STT model name");

    std::string body;
    body.reserve(wav.size() + model.size() + boundary.size() * 4 + 256);
    const auto text = [&body](std::string_view value) {
        body.append(value.data(), value.size());
    };
    text("--");
    text(boundary);
    text("\r\nContent-Disposition: form-data; name=\"file\"; filename=\"dictation.wav\"\r\n"
         "Content-Type: audio/wav\r\n\r\n");
    if (!wav.empty())
        body.append(reinterpret_cast<const char*>(wav.data()), wav.size());
    text("\r\n--");
    text(boundary);
    text("\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n");
    text(model);
    text("\r\n--");
    text(boundary);
    text("--\r\n");
    return body;
}

} // namespace monkeybyte
