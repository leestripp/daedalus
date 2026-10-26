// Unit tests for MonkeyByte core logic (no GTK main loop required).
// Simple assert-based runner (design §14: tests for WorkspaceTree, Markdown,
// SettingsStore, FileService).

#include "monkeybyte/backend/FileService.h"
#include "monkeybyte/backend/AudioEngine.h"
#include "monkeybyte/backend/FileKind.h"
#include "monkeybyte/backend/FileTemplates.h"
#include "monkeybyte/backend/ChimpFlowClient.h"
#include "monkeybyte/backend/HtmlToText.h"
#include "monkeybyte/backend/MarkdownToHtml.h"
#include "monkeybyte/backend/McpBridge.h"
#include "monkeybyte/backend/ModelBridge.h"
#include "monkeybyte/backend/ProjectTypes.h"
#include "monkeybyte/backend/SearchService.h"
#include "monkeybyte/backend/Wav.h"
#include "monkeybyte/backend/SettingsStore.h"
#include "monkeybyte/backend/SpeechStream.h"
#include "monkeybyte/backend/VoiceService.h"
#include "monkeybyte/backend/SkillDiscovery.h"
#include "monkeybyte/tree/WorkspaceTree.h"
#include "monkeybyte/ui/FileIcons.h"
#include "monkeybyte/ui/HtmlView.h"
#include "monkeybyte/ui/chat/Markdown.h"
#include "monkeybyte/ui/chat/ScrollPin.h"
#include "monkeybyte/ui/chat/UserCommands.h"

#include <gdkmm/display.h>
#include <gtkmm/application.h>
#include <gtkmm/box.h>
#include <gtkmm/window.h>
#include <giomm/init.h>
#include <glibmm/init.h>

#include <daedalus/settings.h>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <glibmm/main.h>
#include <fstream>
#include <iostream>
#include <string>
#include <limits>
#include <thread>
#include <unordered_set>

namespace {

int failures = 0;

void check(bool cond, const std::string& name) {
    if (cond) {
        std::cout << "  ok: " << name << '\n';
    } else {
        std::cerr << "FAIL: " << name << '\n';
        ++failures;
    }
}

namespace fs = std::filesystem;

fs::path make_temp_dir(const std::string& name) {
    const fs::path dir = fs::temp_directory_path() / ("monkeybyte_test_" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

// ── FileKind (Plan.md — system file associations) ───────────────────
void test_file_kind() {
    std::cout << "FileKind\n";
    const auto dir = make_temp_dir("filekind");

    const auto write = [&](const std::string& name, const std::string& data) {
        const fs::path p = dir / name;
        std::ofstream f(p, std::ios::binary);
        f << data;
        f.close();
        return p.string();
    };

    check(monkeybyte::is_text_like_file(write("a.txt", "hello world\n")),
          "plain .txt is text-like");
    check(monkeybyte::is_text_like_file(write("b.md", "# Title\n")),
          ".md is text-like");
    check(monkeybyte::is_text_like_file(write("empty", "")),
          "empty file is text-like");

    const std::string bin = std::string("RIFF", 4) + std::string(1, '\0') +
                            std::string(100, '\x01');
    check(!monkeybyte::is_text_like_file(write("c.bin", bin)),
          "binary with NULs is not text-like");
    check(monkeybyte::is_text_like_file(write("d.monkeybyteext", "plain text\n")),
          "unknown extension with text content is text-like");
    check(!monkeybyte::is_text_like_file(write("e.monkeybyteext", bin)),
          "unknown extension with binary content is not text-like");

    // Negative launch path: nonexistent file must fail cleanly, not throw.
    std::string err;
    check(!monkeybyte::launch_external((dir / "missing.mp4").string(), err),
          "launch_external on missing file fails");
    check(!err.empty(), "launch_external reports an error");

    fs::remove_all(dir);
}

// ── FileService (§10) ───────────────────────────────────────────────
void test_file_service() {
    std::cout << "FileService\n";
    const auto dir = make_temp_dir("files");
    monkeybyte::FileService svc(dir);

    check(svc.write("hello.txt", "world").has_value(), "write file");
    auto content = svc.read("hello.txt");
    check(content && *content == "world", "read back content");

    check(svc.mkdir("sub/deep").has_value(), "mkdir nested");
    check(svc.write("sub/deep/x.cpp", "int main(){}").has_value(), "write nested");

    auto root = svc.list("");
    check(root && root->size() == 2, "list root has 2 entries");
    check(root && root->front().is_dir, "dirs sorted first (§4.2)");

    // sandbox: escape attempts rejected
    check(!svc.read("../outside.txt").has_value(), "reject ../ escape");
    check(!svc.write("/etc/monkeybyte_evil", "x").has_value(), "reject absolute escape");
    check(svc.resolve("sub/../hello.txt").has_value(), "resolve normalizes inside");

    check(svc.remove("hello.txt").has_value(), "delete file");
    check(!svc.read("hello.txt").has_value(), "deleted file unreadable");

    // rename (tree context menu)
    check(svc.write("old.txt", "data").has_value(), "write for rename");
    check(svc.rename("old.txt", "new.txt").has_value(), "rename file");
    check(!svc.read("old.txt").has_value(), "old name gone after rename");
    check(svc.read("new.txt") && *svc.read("new.txt") == "data",
          "new name has content");
    check(!svc.rename("new.txt", "../escape.txt").has_value(),
          "reject rename escape");
    check(!svc.rename("/etc/passwd", "x.txt").has_value(),
          "reject rename from outside workspace");

    // hidden filtering (Explorer "Show Hidden Files" setting)
    check(svc.write(".hidden_file", "x").has_value(), "write dotfile");
    {
        auto l = svc.list("");
        bool found = false;
        if (l)
            for (const auto& e : *l)
                if (e.name == ".hidden_file") found = true;
        check(!found, "dotfiles hidden by default");
    }
    svc.set_show_hidden(true);
    {
        auto l = svc.list("");
        bool found = false;
        if (l)
            for (const auto& e : *l)
                if (e.name == ".hidden_file") found = true;
        check(found, "dotfiles listed when show_hidden enabled");
    }

    fs::remove_all(dir);
}


// ── ChimpFlow settings + HTTP contract (§CHIMPFLOW) ─────────────────
void test_chimpflow_settings_and_client() {
    std::cout << "ChimpFlow settings/client\n";
    using monkeybyte::ChimpFlowClient;
    using monkeybyte::ChimpFlowError;
    using monkeybyte::ChimpFlowSettings;
    using monkeybyte::HttpResponse;

    const auto dir = make_temp_dir("chimpflow_settings");
    monkeybyte::SettingsStore store((dir / "settings.db").string());
    const auto defaults = store.load_chimpflow();
    check(!defaults.enabled && defaults.read_assistant && defaults.dictation_enabled &&
              defaults.server_url == "http://127.0.0.1" && defaults.port == 8090 &&
              defaults.api_key.empty() && defaults.tts_model == "pocket-tts" &&
              defaults.stt_model == "whisper" && defaults.voice.empty(),
          "ChimpFlow safe defaults");

    ChimpFlowSettings settings;
    settings.enabled = true;
    settings.read_assistant = false;
    settings.dictation_enabled = false;
    settings.server_url = "http://localhost/";
    settings.port = 18090;
    settings.api_key = "test-key";
    settings.tts_model = "pocket-tts-test";
    settings.stt_model = "whisper-test";
    settings.voice = "alba";
    store.save_chimpflow(settings);
    const auto loaded = store.load_chimpflow();
    check(loaded.enabled && !loaded.read_assistant && !loaded.dictation_enabled &&
              loaded.server_url == "http://localhost/" && loaded.port == 18090 &&
              loaded.api_key == "test-key" && loaded.tts_model == "pocket-tts-test" &&
              loaded.stt_model == "whisper-test" && loaded.voice == "alba",
          "ChimpFlow every field round-trips");
    store.set("chimpflow.port", "not-a-number");
    check(store.load_chimpflow().port == 8090, "ChimpFlow corrupt port uses default");
    store.save_chimpflow(settings);

    check(monkeybyte::chimpflow_base_url(settings) == "http://localhost:18090",
          "ChimpFlow URL strips trailing slash and appends port");
    settings.server_url = "http://[::1]";
    check(monkeybyte::chimpflow_base_url(settings) == "http://[::1]:18090",
          "ChimpFlow IPv6 loopback URL");
    const std::vector<std::string> rejected = {
        "https://127.0.0.1", "http://example.com", "http://127.0.0.1:8090",
        "http://127.0.0.1/v1", "http://user@127.0.0.1", "http://0.0.0.0"};
    for (const auto& url : rejected) {
        settings.server_url = url;
        bool threw = false;
        try {
            (void)monkeybyte::chimpflow_base_url(settings);
        } catch (const ChimpFlowError&) {
            threw = true;
        }
        check(threw, "ChimpFlow rejects non-loopback/ambiguous URL: " + url);
    }
    settings.server_url = "http://127.0.0.1";
    settings.port = 0;
    bool bad_port = false;
    try {
        (void)monkeybyte::chimpflow_base_url(settings);
    } catch (const ChimpFlowError&) {
        bad_port = true;
    }
    check(bad_port, "ChimpFlow rejects port zero");
    fs::remove_all(dir);
}

monkeybyte::HttpResponse test_http_response(long status, const std::string& body) {
    monkeybyte::HttpResponse response;
    response.status = status;
    response.body.assign(body.begin(), body.end());
    return response;
}

void test_chimpflow_http_contract() {
    std::cout << "ChimpFlow HTTP contract\n";
    using monkeybyte::ChimpFlowClient;
    using monkeybyte::ChimpFlowError;
    using monkeybyte::ChimpFlowSettings;
    using monkeybyte::HttpRequest;
    using monkeybyte::HttpResponse;

    ChimpFlowSettings settings;
    settings.api_key = "test-key";
    settings.voice = "alba";
    std::vector<HttpRequest> requests;
    ChimpFlowClient client([&](const HttpRequest& request, std::stop_token) {
        requests.push_back(request);
        if (request.url.ends_with("/healthz"))
            return test_http_response(200, R"({"status":"ok","service":"chimpflow"})");
        if (request.url.ends_with("/v1/models"))
            return test_http_response(200, R"({"data":[{"id":"pocket-tts"}]})");
        if (request.url.ends_with("/v1/voices"))
            return test_http_response(200, R"({"data":[{"id":"alba"},{"id":"anna"}]})");
        if (request.url.ends_with("/v1/audio/speech"))
            return test_http_response(200, std::string("A\0B\x01", 4));
        if (request.url.ends_with("/v1/audio/transcriptions"))
            return test_http_response(200, R"({"text":"  hello agent \n"})");
        return test_http_response(404, R"({"error":"unexpected route"})");
    });

    client.health(settings);
    check(client.list_models(settings) == std::vector<std::string>{"pocket-tts"},
          "model discovery parses runtime ids");
    check(client.list_voices(settings) == (std::vector<std::string>{"alba", "anna"}),
          "voice discovery parses runtime ids");
    const auto speech = client.synthesize(settings, "Hello from MonkeyByte", "alba");
    check(speech == (std::vector<std::uint8_t>{'A', 0, 'B', 1}),
          "TTS response remains binary-safe");
    const std::vector<float> stt_samples = {0.0f, 0.5f, -0.5f};
    const auto wav = monkeybyte::make_wav_16k_mono(stt_samples);
    check(client.transcribe(settings, wav) == "hello agent",
          "STT response text is trimmed");
    check(requests.size() == 5, "all five API calls used the transport");
    check(requests[0].headers.empty(), "health does not require auth");
    for (std::size_t i = 1; i < requests.size(); ++i) {
        const auto& headers = requests[i].headers;
        check(headers.size() == 1 && headers[0].first == "Authorization" &&
                  headers[0].second == "Bearer test-key",
              "authenticated endpoint uses bearer key " + std::to_string(i));
    }
    check(requests[1].url == "http://127.0.0.1:8090/v1/models" &&
              requests[2].url == "http://127.0.0.1:8090/v1/voices",
          "discovery URLs are exact");
    const auto tts = nlohmann::json::parse(requests[3].body);
    check(tts.at("model") == "pocket-tts" && tts.at("voice") == "alba" &&
              tts.at("input") == "Hello from MonkeyByte" &&
              tts.at("response_format") == "wav" && requests[3].method == "POST",
          "TTS request matches OpenAI-compatible contract");
    const auto& multipart = requests[4].body;
    const std::string wav_bytes(reinterpret_cast<const char*>(wav.data()), wav.size());
    check(multipart.find(wav_bytes) != std::string::npos &&
              multipart.find("name=\"model\"\r\n\r\nwhisper\r\n") != std::string::npos &&
              requests[4].content_type.find("boundary=") != std::string::npos,
          "STT multipart preserves WAV bytes and includes model");

    for (const long status : {401L, 403L, 502L, 503L}) {
        ChimpFlowClient failing([&](const HttpRequest&, std::stop_token) {
            return test_http_response(status, R"({"error":"specific gateway error"})");
        });
        bool caught = false;
        try {
            (void)failing.list_voices(settings);
        } catch (const ChimpFlowError& e) {
            caught = e.status() == status &&
                     std::string(e.what()).find("specific gateway error") != std::string::npos;
        }
        check(caught, "gateway error surfaces status/detail " + std::to_string(status));
    }
    ChimpFlowClient malformed([](const HttpRequest&, std::stop_token) {
        return test_http_response(200, "not-json");
    });
    bool bad_json = false;
    try {
        (void)malformed.list_models(settings);
    } catch (const ChimpFlowError& e) {
        bad_json = std::string(e.what()).find("invalid JSON") != std::string::npos;
    }
    check(bad_json, "malformed discovery JSON is an explicit error");

    bool saw_cancellation = false;
    ChimpFlowClient cancelled([&](const HttpRequest&, std::stop_token token) {
        saw_cancellation = token.stop_requested();
        return test_http_response(200, R"({"data":[]})");
    });
    std::stop_source stop;
    stop.request_stop();
    check(cancelled.list_models(settings, stop.get_token()).empty() && saw_cancellation,
          "stop token reaches the transport");

    settings.api_key.clear();
    const char* old_env = std::getenv("CHIMPFLOW_KEY");
    const std::string saved_env = old_env ? old_env : "";
    ::setenv("CHIMPFLOW_KEY", "environment-key", 1);
    check(monkeybyte::resolved_chimpflow_api_key(settings) == "environment-key",
          "empty stored key falls back to CHIMPFLOW_KEY");
    settings.api_key = "explicit-key";
    check(monkeybyte::resolved_chimpflow_api_key(settings) == "explicit-key",
          "explicit stored key wins over environment");
    if (old_env) ::setenv("CHIMPFLOW_KEY", saved_env.c_str(), 1);
    else ::unsetenv("CHIMPFLOW_KEY");
}

void test_wav_contract() {
    std::cout << "ChimpFlow WAV/multipart\n";
    const std::vector<float> samples = {0.0f, 0.5f, -0.5f, 1.0f, -1.0f};
    const auto wav = monkeybyte::make_wav_16k_mono(samples);
    monkeybyte::WavAudio parsed;
    std::string error;
    check(monkeybyte::parse_wav(wav, parsed, error) && parsed.sample_rate == 16000 &&
              parsed.samples.size() == samples.size() &&
              std::abs(parsed.samples[1] - 0.5f) < 0.0001f &&
              std::abs(parsed.samples[2] + 0.5f) < 0.0001f,
          "16-bit capture WAV round-trip");
    check(wav.size() == 44 + samples.size() * 2, "WAV header/data sizes are exact");

    const auto with_bits = [&](int bits) {
        auto out = wav;
        const auto set16 = [&out](std::size_t p, unsigned value) {
            out[p] = static_cast<std::uint8_t>(value & 0xff);
            out[p + 1] = static_cast<std::uint8_t>(value >> 8);
        };
        const auto set32 = [&out](std::size_t p, std::uint32_t value) {
            for (std::size_t i = 0; i < 4; ++i)
                out[p + i] = static_cast<std::uint8_t>(value >> (i * 8));
        };
        const std::size_t sample_bytes = static_cast<std::size_t>(bits / 8);
        const std::size_t data_bytes = samples.size() * sample_bytes;
        // RIFF chunks are padded to an even byte count (24-bit * five samples
        // is odd); the production parser deliberately rejects a missing pad.
        out.resize(44 + data_bytes + (data_bytes & 1u));
        set32(4, static_cast<std::uint32_t>(out.size() - 8));
        set32(28, 16000u * static_cast<std::uint32_t>(sample_bytes));
        set16(32, static_cast<unsigned>(sample_bytes));
        set16(34, static_cast<unsigned>(bits));
        set32(40, static_cast<std::uint32_t>(samples.size() * sample_bytes));
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const std::size_t p = 44 + i * sample_bytes;
            if (bits == 24) {
                const std::int32_t value = static_cast<std::int32_t>(samples[i] * 8388607.0f);
                const auto raw = static_cast<std::uint32_t>(value);
                out[p] = static_cast<std::uint8_t>(raw);
                out[p + 1] = static_cast<std::uint8_t>(raw >> 8);
                out[p + 2] = static_cast<std::uint8_t>(raw >> 16);
            } else {
                const auto raw = static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(samples[i] * 2147483647.0f));
                for (std::size_t b = 0; b < 4; ++b)
                    out[p + b] = static_cast<std::uint8_t>(raw >> (b * 8));
            }
        }
        return out;
    };
    for (const int bits : {24, 32}) {
        const auto encoded = with_bits(bits);
        check(monkeybyte::parse_wav(encoded, parsed, error) &&
                  parsed.samples.size() == samples.size() &&
                  std::abs(parsed.samples[1] - 0.5f) < 0.0001f &&
                  std::abs(parsed.samples[2] + 0.5f) < 0.0001f,
              std::to_string(bits) + "-bit PCM WAV playback parse");
    }

    auto truncated = wav;
    truncated.pop_back();
    check(!monkeybyte::parse_wav(truncated, parsed, error) && !error.empty(),
          "truncated WAV is rejected");
    auto stereo = wav;
    stereo[22] = 2;
    check(!monkeybyte::parse_wav(stereo, parsed, error), "stereo WAV is rejected");
    auto bad_rate = wav;
    bad_rate[24] = bad_rate[25] = bad_rate[26] = bad_rate[27] = 0;
    check(!monkeybyte::parse_wav(bad_rate, parsed, error), "zero sample rate is rejected");

    const auto multipart = monkeybyte::make_stt_multipart("test-boundary", "whisper", wav);
    const std::string wav_bytes(reinterpret_cast<const char*>(wav.data()), wav.size());
    check(multipart.find(wav_bytes) != std::string::npos &&
              multipart.find("filename=\"dictation.wav\"") != std::string::npos &&
              multipart.find("name=\"model\"\r\n\r\nwhisper\r\n") != std::string::npos &&
              multipart.ends_with("--test-boundary--\r\n"),
          "multipart body is binary-safe and complete");
    bool bad_model = false;
    try {
        (void)monkeybyte::make_stt_multipart("test", "bad\r\nmodel", wav);
    } catch (const std::invalid_argument&) {
        bad_model = true;
    }
    check(bad_model, "multipart rejects header injection in model name");
}


// ── ChimpFlow orchestration (§CHIMPFLOW) ────────────────────────────
void test_speech_stream() {
    std::cout << "Speech stream segmentation\n";
    monkeybyte::SpeechStreamSegmenter stream;
    check(stream.append("First sen").empty(),
          "partial sentence stays buffered across deltas");
    auto chunks = stream.append("tence. Second sen");
    check(chunks.size() == 1 && chunks[0] == "First sentence.",
          "first complete sentence streams before the bubble closes");
    chunks = stream.append("tence! Final");
    check(chunks.size() == 1 && chunks[0] == " Second sentence!",
          "second sentence is emitted once in FIFO order");
    check(stream.take_remainder() == " Final" && stream.take_remainder().empty(),
          "bubble close takes only the unspoken remainder");

    chunks = stream.append("**Bold sentence.** Next");
    check(chunks.size() == 1 && chunks[0] == "**Bold sentence.**",
          "Markdown closing markers stay with the streamed sentence");
    check(stream.take_remainder() == " Next", "Markdown tail remains ordered");

    chunks = stream.append("Value 3.14 is fine. Next");
    check(chunks.size() == 1 && chunks[0] == "Value 3.14 is fine.",
          "decimal point is not a sentence boundary");
    check(stream.take_remainder() == " Next", "decimal tail remains for final flush");

    chunks = stream.append("Café 🐒 works! Next");
    check(chunks.size() == 1 && chunks[0] == "Café 🐒 works!",
          "UTF-8 text is split only at a valid sentence boundary");
    check(stream.take_remainder() == " Next", "UTF-8 tail remains intact");

    chunks = stream.append("Wait… Next");
    check(chunks.size() == 1 && chunks[0] == "Wait…",
          "complete UTF-8 ellipsis is a streaming boundary");
    check(stream.take_remainder() == " Next", "ellipsis tail remains intact");

    check(stream.append("Not yet.").empty(),
          "terminal punctuation waits for confirmation or bubble close");
    check(stream.take_remainder() == "Not yet.",
          "unconfirmed terminal sentence is preserved for final narration");

    chunks = stream.append("Before.\n```cpp\nx.y();\n```\nAfter.");
    check(chunks.size() == 1 && chunks[0] == "Before.",
          "prose before a code fence streams independently");
    const auto code_remainder = stream.take_remainder();
    check(code_remainder.find("x.y();") != std::string::npos &&
              code_remainder.find("After.") != std::string::npos,
          "fenced code is not split into misleading partial utterances");

    chunks = stream.append("First paragraph.\n\nSecond");
    check(chunks.size() == 1 && chunks[0] == "First paragraph.",
          "paragraph separators do not create whitespace-only TTS chunks");
    check(stream.take_remainder() == "Second", "paragraph tail remains ordered");

    stream.append("Discard this partial sentence");
    stream.clear();
    check(stream.take_remainder().empty(), "clear drops stale stream state");
}

void test_audio_normalisation() {
    std::cout << "Audio normalisation\n";
    const auto near = [](float actual, float expected) {
        return std::abs(actual - expected) < 0.0005f;
    };
    const auto rms = [](const std::vector<float>& samples) {
        double sum = 0.0;
        for (const float sample : samples) sum += sample * sample;
        return std::sqrt(sum / samples.size());
    };

    std::vector<float> quiet{0.01f, -0.01f};
    monkeybyte::normalise_playback_samples(quiet);
    check(near(quiet[0], 0.04f) && near(quiet[1], -0.04f),
          "quiet voice gain is capped at 4x instead of boosting noise without limit");

    std::vector<float> medium{0.05f, -0.05f};
    monkeybyte::normalise_playback_samples(medium);
    check(near(medium[0], 0.12f) && std::abs(rms(medium) - 0.12) < 0.0005,
          "medium voice reaches the RMS loudness target");

    std::vector<float> loud{0.5f, -0.5f};
    monkeybyte::normalise_playback_samples(loud);
    check(near(loud[0], 0.12f) && std::abs(rms(loud) - 0.12) < 0.0005,
          "loud voice is attenuated to the same RMS target");

    std::vector<float> spiky(1024, 0.001f);
    spiky[0] = 1.0f;
    monkeybyte::normalise_playback_samples(spiky);
    check(near(spiky[0], 0.98f) &&
              std::all_of(spiky.begin(), spiky.end(),
                          [](float sample) { return std::abs(sample) <= 0.98001f; }),
          "peak limiter prevents clipping on transient-heavy voices");

    std::vector<float> silence(8, 0.0f);
    monkeybyte::normalise_playback_samples(silence);
    check(std::all_of(silence.begin(), silence.end(),
                      [](float sample) { return sample == 0.0f; }),
          "digital silence remains silent");

    std::vector<float> nonfinite{std::numeric_limits<float>::quiet_NaN(),
                                 std::numeric_limits<float>::infinity(),
                                 0.05f, -0.05f};
    monkeybyte::normalise_playback_samples(nonfinite);
    check(nonfinite[0] == 0.0f && nonfinite[1] == 0.0f &&
              std::all_of(nonfinite.begin(), nonfinite.end(),
                          [](float sample) { return std::isfinite(sample); }) &&
              std::abs(rms(nonfinite) - 0.12) < 0.0005,
          "non-finite samples are sanitized before gain calculation");
}

class FakeAudioBackend final : public monkeybyte::AudioBackend {
public:
    void start_recording() override {
        ++record_starts;
        recording = true;
    }
    std::vector<float> stop_recording() override {
        ++record_stops;
        recording = false;
        return recorded_samples;
    }
    void play(std::uint32_t rate, std::vector<float> samples,
              std::function<void()> on_drained) override {
        ++play_calls;
        last_rate = rate;
        last_samples = std::move(samples);
        drained = std::move(on_drained);
    }
    void stop_playback() override {
        ++playback_stops;
        drained = {};
    }
    void complete() {
        auto callback = std::move(drained);
        drained = {};
        if (callback) callback();
    }

    int record_starts = 0;
    int record_stops = 0;
    int play_calls = 0;
    int playback_stops = 0;
    bool recording = false;
    std::uint32_t last_rate = 0;
    std::vector<float> last_samples;
    std::vector<float> recorded_samples;
    std::function<void()> drained;
};

bool pump_voice_until(const std::function<bool()>& predicate) {
    auto context = Glib::MainContext::get_default();
    for (int i = 0; i < 3000; ++i) {
        if (predicate()) return true;
        while (context->iteration(false)) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

void test_chimpflow_voice_service() {
    std::cout << "ChimpFlow VoiceService\n";
    using monkeybyte::ChimpFlowClient;
    using monkeybyte::ChimpFlowSettings;
    using monkeybyte::DictationState;
    using monkeybyte::VoiceCatalog;
    using monkeybyte::VoiceService;

    const std::string plain = monkeybyte::speech_text(
        "# Hello\n\n**world** [link](https://example.invalid) "
        "![image](image.png)\n\n```cpp\nanswer();\n```\n\nMonkey 🐒");
    check(plain.find("Hello") != std::string::npos &&
              plain.find("world") != std::string::npos &&
              plain.find("link") != std::string::npos &&
              plain.find("answer();") != std::string::npos &&
              plain.find("Monkey 🐒") != std::string::npos &&
              plain.find("https://") == std::string::npos &&
              plain.find("```") == std::string::npos,
          "speech_text preserves prose/UTF-8 and drops Markdown wiring");

    struct FakeState {
        std::mutex mutex;
        std::vector<std::string> spoken;
        int transcription_calls = 0;
        bool fail_next_speech = false;
    };
    auto state = std::make_shared<FakeState>();
    auto client = std::make_shared<ChimpFlowClient>(
        [state](const monkeybyte::HttpRequest& request, std::stop_token) {
            std::lock_guard lock(state->mutex);
            if (request.url.ends_with("/healthz"))
                return test_http_response(200, R"({"status":"ok"})");
            if (request.url.ends_with("/v1/models"))
                return test_http_response(200, R"({"data":[{"id":"pocket-tts"}]})");
            if (request.url.ends_with("/v1/voices"))
                return test_http_response(200, R"({"data":[{"id":"alba"},{"id":"anna"}]})");
            if (request.url.ends_with("/v1/audio/speech")) {
                const auto json = nlohmann::json::parse(request.body);
                state->spoken.push_back(json.at("input").get<std::string>());
                if (state->fail_next_speech) {
                    state->fail_next_speech = false;
                    return test_http_response(502, R"({"error":"engine test failure"})");
                }
                const auto wav = monkeybyte::make_wav_16k_mono(
                    std::vector<float>{0.1f, 0.2f});
                return test_http_response(
                    200, std::string(reinterpret_cast<const char*>(wav.data()),
                                     wav.size()));
            }
            if (request.url.ends_with("/v1/audio/transcriptions")) {
                ++state->transcription_calls;
                return test_http_response(200, R"({"text":"  dictated text  "})");
            }
            return test_http_response(404, R"({"error":"unexpected route"})");
        });

    const auto dir = make_temp_dir("voice_service");
    monkeybyte::SettingsStore store((dir / "settings.db").string());
    ChimpFlowSettings config;
    config.enabled = true;
    config.api_key = "test-key";
    config.voice = "alba";
    store.save_chimpflow(config);
    auto audio = std::make_shared<FakeAudioBackend>();
    VoiceService service(store, client, audio);

    VoiceCatalog catalog;
    std::string discovery_error = "pending";
    service.discover(config, [&](const VoiceCatalog& result, const std::string& error) {
        catalog = result;
        discovery_error = error;
    });
    check(pump_voice_until([&] { return discovery_error != "pending"; }) &&
              discovery_error.empty() &&
              catalog.models == std::vector<std::string>{"pocket-tts"} &&
              catalog.voices == (std::vector<std::string>{"alba", "anna"}),
          "discovery returns models/voices on the main context");

    std::string transcript;
    std::string last_error;
    service.signal_dictation_text().connect(
        [&](const std::string& text) { transcript = text; });
    service.signal_error().connect(
        [&](const std::string& error) { last_error = error; });

    service.speak("**First** message");
    service.speak("Second message");
    check(pump_voice_until([&] { return audio->play_calls == 1; }),
          "first assistant utterance reaches playback");
    check(audio->play_calls == 1 && audio->last_rate == 16000 &&
              audio->last_samples.size() == 2,
          "second utterance waits for first drained callback");
    audio->complete();
    check(pump_voice_until([&] { return audio->play_calls == 2; }),
          "drained playback advances FIFO speech queue");
    audio->complete();
    check(pump_voice_until([&] { return !service.speech_active(); }),
          "speech queue drains to idle");
    {
        std::lock_guard lock(state->mutex);
        check(state->spoken.size() == 2 && state->spoken[0] == "First message" &&
                  state->spoken[1] == "Second message",
              "assistant speech is Markdown-clean and FIFO");
    }

    state->fail_next_speech = true;
    service.speak("Failing utterance");
    service.speak("Recovery utterance");
    check(pump_voice_until([&] { return audio->play_calls == 3; }),
          "one failed TTS item does not block later speech");
    check(last_error.find("engine test failure") != std::string::npos,
          "assistant synthesis failure is visible");
    audio->complete();
    check(pump_voice_until([&] { return !service.speech_active(); }),
          "recovery utterance completes");

    bool preview_done = false;
    std::string preview_error = "pending";
    service.preview(config, "alba", [&](const std::string& error) {
        preview_error = error;
        preview_done = true;
    });
    check(pump_voice_until([&] { return audio->play_calls == 4; }),
          "voice preview reaches playback");
    audio->complete();
    check(pump_voice_until([&] { return preview_done; }) && preview_error.empty(),
          "voice preview reports drained completion");

    audio->recorded_samples.assign(5000, 0.1f);
    service.toggle_dictation();
    check(service.dictation_state() == DictationState::Recording && audio->recording,
          "first mic toggle starts recording");
    service.speak("Queued while recording");
    check(audio->play_calls == 4, "recording suppresses speaker feedback");
    service.toggle_dictation();
    check(service.dictation_state() == DictationState::Transcribing && !audio->recording,
          "second mic toggle stops recording and starts STT");
    check(pump_voice_until([&] {
              return service.dictation_state() == DictationState::Idle &&
                     transcript == "dictated text";
          }),
          "dictation returns trimmed review text without auto-send");
    check(pump_voice_until([&] { return audio->play_calls == 5; }),
          "speech queued during dictation resumes afterward");
    audio->complete();

    service.speak("Cancel active speech");
    check(pump_voice_until([&] { return audio->play_calls == 6; }),
          "speech active before stop_all");
    service.stop_all();
    check(!service.speech_active() && service.queued_speech() == 0,
          "stop_all cancels active and queued speech");
    audio->complete(); // stale/no callback after intentional cancellation
    check(!service.speech_active(), "stale drained callback cannot resurrect speech");

    config.enabled = false;
    store.save_chimpflow(config);
    service.reload();
    const auto spoken_before = [&] {
        std::lock_guard lock(state->mutex);
        return state->spoken.size();
    }();
    service.speak("Must stay silent");
    check(state->spoken.size() == spoken_before && !service.speech_active(),
          "master disable stops assistant narration");
    service.toggle_dictation();
    check(last_error.find("Enable ChimpFlow") != std::string::npos &&
              service.dictation_state() == DictationState::Idle,
          "disabled dictation fails with actionable feedback");

    config.enabled = true;
    store.save_chimpflow(config);
    service.reload();
    audio->recorded_samples = {0.0f};
    transcript = "unchanged";
    service.toggle_dictation();
    service.toggle_dictation();
    check(service.dictation_state() == DictationState::Idle && transcript.empty(),
          "short recording returns empty transcript without STT");

    bool cancelled_preview = false;
    std::string cancelled_error;
    service.preview(config, "alba", [&](const std::string& error) {
        cancelled_preview = true;
        cancelled_error = error;
    });
    check(pump_voice_until([&] { return audio->play_calls == 7; }),
          "second preview active before cancellation");
    service.stop_all();
    check(cancelled_preview && cancelled_error.find("cancelled") != std::string::npos,
          "cancelling preview completes its callback and clears UI busy state");

    config.stt_model.clear();
    store.save_chimpflow(config);
    service.reload();
    const int record_starts_before = audio->record_starts;
    service.toggle_dictation();
    check(service.dictation_state() == DictationState::Idle &&
              // Virtual call through VoiceService is opaque to cppcheck.
              audio->record_starts == record_starts_before && // cppcheck-suppress knownConditionTrueFalse
              last_error.find("STT model") != std::string::npos,
          "dictation rejects missing required configuration before opening mic");
    service.stop_all();
    fs::remove_all(dir);
}

// ── SettingsStore (§10.2) ───────────────────────────────────────────
void test_settings_store() {
    std::cout << "SettingsStore\n";
    const auto dir = make_temp_dir("settings");
    monkeybyte::SettingsStore store((dir / "test.db").string());

    store.set("workspace", "/tmp/ws");
    check(store.get("workspace").value_or("") == "/tmp/ws", "kv round-trip");
    check(!store.get("missing").has_value(), "missing key empty");

    monkeybyte::InterfaceSettings iface;
    iface.font_size = 16;
    iface.tab_size = 4;
    iface.word_wrap = false;
    iface.theme = "light";
    iface.browser_home_url = "http://localhost:3000";
    store.save_interface(iface);
    const auto loaded = store.load_interface();
    check(loaded.font_size == 16 && loaded.tab_size == 4 && !loaded.word_wrap &&
              loaded.theme == "light" &&
              loaded.browser_home_url == "http://localhost:3000",
          "interface round-trip");

    iface.show_hidden = true;
    iface.show_line_numbers = false;
    iface.highlight_current_line = false;
    store.save_interface(iface);
    const auto loaded2 = store.load_interface();
    check(loaded2.show_hidden && !loaded2.show_line_numbers &&
              !loaded2.highlight_current_line,
          "interface new fields round-trip");

    // chat appearance overrides (§4.5 Chat group)
    check(loaded2.chat_font_size == 0 && loaded2.chat_bg.empty() &&
              loaded2.chat_user_bg.empty() && loaded2.chat_user_text.empty() &&
              loaded2.chat_assistant_bg.empty() &&
              loaded2.chat_assistant_text.empty(),
          "chat appearance defaults to theme (0 / empty)");
    iface.chat_font_size = 15;
    iface.chat_bg = "#101010";
    iface.chat_user_bg = "#ff0000";
    iface.chat_user_text = "#00ff00";
    iface.chat_assistant_bg = "#0000ff";
    iface.chat_assistant_text = "#ffffff";
    store.save_interface(iface);
    const auto loaded3 = store.load_interface();
    check(loaded3.chat_font_size == 15 && loaded3.chat_bg == "#101010" &&
              loaded3.chat_user_bg == "#ff0000" &&
              loaded3.chat_user_text == "#00ff00" &&
              loaded3.chat_assistant_bg == "#0000ff" &&
              loaded3.chat_assistant_text == "#ffffff",
          "chat appearance round-trip");

    // search excludes: seeded with defaults on first load, then round-trip
    const auto seeded = store.load_search_excludes();
    check(seeded == monkeybyte::SettingsStore::default_search_excludes(),
          "search excludes seeded with defaults");
    check(store.get("search.excludes").has_value(),
          "search excludes persisted in saved settings");
    store.save_search_excludes({"vendor", "*.gen"});
    const auto custom = store.load_search_excludes();
    check(custom.size() == 2 && custom[0] == "vendor" && custom[1] == "*.gen",
          "search excludes round-trip");

    // model_prompts binding (§4.5), keyed by the daedalus model name
    check(store.get_model_system_prompt("some-model") == 0,
          "unbound model prompt is 0");
    monkeybyte::SystemPrompt p;
    p.name = "Reviewer";
    p.prompt = "Review the code.";
    p.is_default = true;
    store.save_system_prompt(p);
    auto prompts = store.list_system_prompts();
    check(prompts.size() == 1 && prompts[0].is_default && prompts[0].id > 0,
          "system prompt insert + id");

    store.set_model_system_prompt("some-model", prompts[0].id);
    check(store.get_model_system_prompt("some-model") == prompts[0].id,
          "model system-prompt binding");
    store.set_model_system_prompt("some-model", 0);
    check(store.get_model_system_prompt("some-model") == 0,
          "model system-prompt unbind");

    // agent host-policy disabled lists (§4.5 Agent tab)
    check(store.load_disabled_skills().empty(), "disabled skills empty");
    store.save_disabled_skills({"firefox", "memory-leak"});
    check(store.load_disabled_skills().size() == 2 &&
              store.load_disabled_skills()[0] == "firefox",
          "disabled skills round-trip");
    store.save_disabled_skills({});
    check(store.load_disabled_skills().empty(), "disabled skills cleared");
    store.save_disabled_tools({"execute_bash"});
    check(store.load_disabled_tools().size() == 1 &&
              store.load_disabled_tools()[0] == "execute_bash",
          "disabled tools round-trip");

    store.delete_system_prompt(prompts[0].id);
    check(store.list_system_prompts().empty(), "prompt delete");

    fs::remove_all(dir);
}

// ── McpBridge (daedalus registry) ────────────────────────────────────
void test_mcp_bridge() {
    std::cout << "McpBridge\n";
    const fs::path dir = fs::temp_directory_path() / "monkeybyte_test_mcp_bridge";
    fs::remove_all(dir);
    fs::create_directories(dir);

    auto store = daedalus::SettingsStore::for_home(dir);

    monkeybyte::McpServer srv;
    srv.name = "filesystem";
    srv.command = "npx -y @modelcontextprotocol/server-filesystem /tmp";
    srv.env_json = "{\"MY_KEY\":\"v\"}";
    monkeybyte::mcp_bridge::save(store, srv);

    auto servers = monkeybyte::mcp_bridge::list(store);
    check(servers.size() == 1 && servers[0].enabled, "bridge round-trip");
    check(servers[0].command == srv.command, "bridge command join");

    // argv split reaches the daedalus model
    auto raw = store.get_mcp_server("filesystem");
    check(raw && raw->command == "npx" && raw->args.size() == 3 &&
              raw->env.at("MY_KEY") == "v",
          "bridge argv + env conversion");

    // edit preserves fields the UI doesn't model (description/transport)
    if (raw) {
        auto d = *raw;
        d.description = "keep me";
        store.upsert_mcp_server(d);
    }
    srv.command = "npx -y @other/server";
    monkeybyte::mcp_bridge::save(store, srv);
    raw = store.get_mcp_server("filesystem");
    check(raw && raw->description == "keep me" && raw->command == "npx" &&
              raw->args.size() == 2,
          "bridge edit preserves unmodeled fields");

    monkeybyte::mcp_bridge::remove(store, "filesystem");
    check(monkeybyte::mcp_bridge::list(store).empty(), "bridge remove");

    // per-server enable/disable toggle (Settings → MCP Servers switch)
    monkeybyte::mcp_bridge::save(store, srv);
    auto toggled = monkeybyte::mcp_bridge::list(store);
    check(toggled.size() == 1 && toggled[0].enabled, "server re-added enabled");
    toggled[0].enabled = false;
    monkeybyte::mcp_bridge::save(store, toggled[0]);
    toggled = monkeybyte::mcp_bridge::list(store);
    check(toggled.size() == 1 && !toggled[0].enabled, "disable round-trip");
    monkeybyte::mcp_bridge::remove(store, "filesystem");

    fs::remove_all(dir);
}

// ── McpBridge::parse_json (raw provider JSON import, §5.4) ───────────
void test_mcp_json_import() {
    std::cout << "McpBridge::parse_json\n";

    // Claude/VS Code "mcpServers" wrapper with two stdio servers
    {
        const std::string text = R"({
            "mcpServers": {
                "filesystem": {
                    "command": "npx",
                    "args": ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"],
                    "env": { "MY_KEY": "v" }
                },
                "github": { "command": "docker", "args": ["run", "gh-mcp"] }
            }
        })";
        auto parsed = monkeybyte::mcp_bridge::parse_json(text);
        check(parsed.has_value(), "json: mcpServers wrapper parses");
        check(parsed && parsed->size() == 2, "json: wrapper yields 2 servers");
        if (parsed && parsed->size() == 2) {
            const auto& fs_srv = (*parsed)[0];
            check(fs_srv.name == "filesystem" && fs_srv.transport == "stdio" &&
                      fs_srv.command == "npx" && fs_srv.args.size() == 3 &&
                      fs_srv.args[0] == "-y" &&
                      fs_srv.env.at("MY_KEY") == "v" && fs_srv.enabled,
                  "json: stdio fields");
            check((*parsed)[1].name == "github" &&
                      (*parsed)[1].args.size() == 2,
                  "json: second server");
        }
    }

    // Single keyed object: { "name": { ...config... } }
    {
        auto parsed = monkeybyte::mcp_bridge::parse_json(
            R"({ "sqlite": { "command": "uvx", "args": ["mcp-server-sqlite"] } })");
        check(parsed && parsed->size() == 1 &&
                  (*parsed)[0].name == "sqlite" &&
                  (*parsed)[0].command == "uvx",
              "json: single keyed object");
    }

    // Bare single-server config with explicit "name"
    {
        auto parsed = monkeybyte::mcp_bridge::parse_json(
            R"({ "name": "bare", "command": "node", "args": ["srv.js"],
                 "enabled": false })");
        check(parsed && parsed->size() == 1 &&
                  (*parsed)[0].name == "bare" &&
                  !(*parsed)[0].enabled,
              "json: bare config with name");
    }

    // HTTP endpoint: url + Authorization header → streamable-http + bearer
    {
        auto parsed = monkeybyte::mcp_bridge::parse_json(R"({
            "mcpServers": {
                "remote": {
                    "url": "https://example.com/mcp",
                    "headers": { "Authorization": "Bearer tok123" }
                }
            }
        })");
        check(parsed && parsed->size() == 1 &&
                  (*parsed)[0].transport == "streamable-http" &&
                  (*parsed)[0].url == "https://example.com/mcp" &&
                  (*parsed)[0].bearer_token == "tok123",
              "json: http url + bearer token");
    }

    // Error cases
    check(!monkeybyte::mcp_bridge::parse_json("{ not json").has_value(),
          "json: invalid JSON rejected");
    check(!monkeybyte::mcp_bridge::parse_json(R"({ "mcpServers": {} })")
              .has_value(),
          "json: empty mcpServers rejected");
    check(!monkeybyte::mcp_bridge::parse_json(
              R"({ "command": "npx" })")
              .has_value(),
          "json: bare config without name rejected");
    check(!monkeybyte::mcp_bridge::parse_json(
              R"({ "mcpServers": { "broken": {} } })")
              .has_value(),
          "json: server without command/url rejected");
}

// ── ModelBridge (daedalus provider/model registry, §4.5/§10.2) ───────
void test_model_bridge() {
    std::cout << "ModelBridge\n";
    const fs::path dir = fs::temp_directory_path() / "monkeybyte_test_model_bridge";
    fs::remove_all(dir);
    fs::create_directories(dir);

    auto registry = daedalus::SettingsStore::for_home(dir);
    monkeybyte::SettingsStore prompts((dir / "mb.db").string());

    // provider CRUD
    monkeybyte::UiProvider p;
    p.name = "TestProv";
    p.host = "https://api.test";
    p.api_key = "${TEST_KEY}";
    monkeybyte::model_bridge::save_provider(registry, p);
    auto providers = monkeybyte::model_bridge::list_providers(registry);
    check(providers.size() == 1 && providers[0].host == "https://api.test" &&
              providers[0].chat_endpoint == "/v1/chat/completions",
          "provider round-trip");

    // §ProviderPlugins — plugin column round-trips; unset stays empty
    // (daedalus auto-detects from name/host).
    check(providers[0].plugin.empty(), "plugin defaults to auto-detect");
    p.plugin = "minimax";
    monkeybyte::model_bridge::save_provider(registry, p);
    providers = monkeybyte::model_bridge::list_providers(registry);
    check(providers.size() == 1 && providers[0].plugin == "minimax",
          "plugin round-trip");

    // model referencing the provider by name
    monkeybyte::UiModel m;
    m.name = "test-1";
    m.provider = "TestProv";
    m.model_id = "Test-Model-1";
    m.max_context_size = 8192;
    m.temperature = 0.3;
    m.is_default = true;
    monkeybyte::model_bridge::save_model(registry, m);
    auto models = monkeybyte::model_bridge::list_models(registry, prompts);
    check(models.size() == 1 && models[0].is_default &&
              models[0].max_context_size == 8192,
          "model round-trip + default flag");
    check(monkeybyte::model_bridge::display_name(models[0]) ==
              "TestProv - Test-Model-1",
          "display name format (§7.1)");

    // resolve_model merges provider host/key (what ChatController uses)
    const auto resolved = registry.resolve_model("test-1");
    check(resolved && resolved->host == "https://api.test" &&
              resolved->api_key == "${TEST_KEY}" &&
              resolved->model_id == "Test-Model-1",
          "resolve_model merges provider");
    check(resolved && resolved->plugin == "minimax",
          "resolve_model carries provider plugin");

    // ${ENV_VAR} placeholder resolution (§4.5 env-key badge)
    setenv("MB_TEST_KEY", "secret", 1);
    check(monkeybyte::model_bridge::resolve_env_key("${MB_TEST_KEY}") == "secret",
          "env key resolved");
    check(monkeybyte::model_bridge::resolve_env_key("plain-key") == "plain-key",
          "plain key passthrough");
    check(monkeybyte::model_bridge::resolve_env_key("${MB_TEST_KEY_UNSET}").empty(),
          "unset env resolves empty");

    // default-model switching (Settings → Models dropdown)
    monkeybyte::UiModel m2 = m;
    m2.name = "test-2";
    m2.model_id = "Test-Model-2";
    m2.is_default = false;
    monkeybyte::model_bridge::save_model(registry, m2);
    monkeybyte::model_bridge::set_default_model(registry, "test-2");
    models = monkeybyte::model_bridge::list_models(registry, prompts);
    bool first_default = true, second_default = false;
    for (const auto& mm : models) {
        if (mm.name == "test-1") first_default = mm.is_default;
        if (mm.name == "test-2") second_default = mm.is_default;
    }
    check(models.size() == 2 && !first_default && second_default,
          "set_default_model switches");

    // system-prompt binding via the bridge (monkeybyte.db model_prompts)
    monkeybyte::SystemPrompt sp;
    sp.name = "Reviewer";
    sp.prompt = "Review.";
    prompts.save_system_prompt(sp);
    const long pid = prompts.list_system_prompts()[0].id;
    monkeybyte::model_bridge::set_model_system_prompt(prompts, "test-1", pid);
    models = monkeybyte::model_bridge::list_models(registry, prompts);
    long bound = -1;
    for (const auto& mm : models)
        if (mm.name == "test-1") bound = mm.system_prompt_id;
    check(bound == pid, "model system-prompt binding");

    // provider delete cascades to its models and their prompt bindings
    monkeybyte::model_bridge::remove_provider(registry, prompts, "TestProv");
    check(monkeybyte::model_bridge::list_models(registry, prompts).empty(),
          "provider delete cascades to models");
    check(prompts.get_model_system_prompt("test-1") == 0,
          "provider delete clears prompt bindings");

    fs::remove_all(dir);
}

// ── ModelBridge legacy migration (monkeybyte.db `models` → daedalus) ─
void test_model_bridge_migration() {
    std::cout << "ModelBridge migration\n";
    const fs::path dir = make_temp_dir("model_bridge_legacy");
    const auto db_path = (dir / "legacy.db").string();

    // Seed a pre-split `models` table (full provider config inline).
    {
        sqlite3* raw = nullptr;
        check(sqlite3_open(db_path.c_str(), &raw) == SQLITE_OK, "legacy db open");
        const char* ddl = R"sql(
CREATE TABLE models(
    provider_name TEXT PRIMARY KEY,
    base_url TEXT NOT NULL,
    endpoint TEXT NOT NULL,
    model_name TEXT NOT NULL,
    api_key TEXT NOT NULL DEFAULT '',
    max_context INTEGER NOT NULL DEFAULT 4096,
    temperature REAL NOT NULL DEFAULT 0.7,
    is_default INTEGER NOT NULL DEFAULT 0,
    system_prompt_id INTEGER NOT NULL DEFAULT 0
);
INSERT INTO models VALUES('OldProv','https://api.old','/v1/chat/completions',
    'old-model','old-key',4096,0.7,1,7);
)sql";
        check(sqlite3_exec(raw, ddl, nullptr, nullptr, nullptr) == SQLITE_OK,
              "legacy models seeded");
        sqlite3_close(raw);
    }

    const fs::path dhome = dir / "daedalus";
    fs::create_directories(dhome);
    auto registry = daedalus::SettingsStore::for_home(dhome);
    {
        monkeybyte::SettingsStore legacy(db_path);
        monkeybyte::model_bridge::migrate_legacy_models(legacy, registry);
    }

    monkeybyte::SettingsStore prompts((dir / "mb.db").string());
    const auto models = monkeybyte::model_bridge::list_models(registry, prompts);
    check(models.size() == 1 && models[0].is_default, "migration moves model");
    check(monkeybyte::model_bridge::display_name(models[0]) ==
              "OldProv - old-model",
          "migration keeps display name");
    const auto resolved = registry.resolve_model("OldProv");
    check(resolved && resolved->host == "https://api.old" &&
              resolved->api_key == "old-key",
          "migration moves provider config");
    {
        monkeybyte::SettingsStore legacy(db_path);
        check(legacy.get_model_system_prompt("OldProv") == 7,
              "migration moves prompt binding");
        check(legacy.read_legacy_models().empty(), "legacy table dropped");
        // idempotent: second run changes nothing
        monkeybyte::model_bridge::migrate_legacy_models(legacy, registry);
    }
    check(monkeybyte::model_bridge::list_models(registry, prompts).size() == 1,
          "migration idempotent");

    fs::remove_all(dir);
}

// ── ModelBridge thinking levels (daedalus §ThinkingLevel) ────────────
void test_model_bridge_thinking_levels() {
    std::cout << "ModelBridge thinking levels\n";
    using monkeybyte::UiModel;
    using monkeybyte::UiProvider;
    namespace mb = monkeybyte::model_bridge;
    using daedalus::ThinkingLevel;
    const fs::path dir = fs::temp_directory_path() / "monkeybyte_test_mb_thinking";
    fs::remove_all(dir);
    fs::create_directories(dir);

    auto registry = daedalus::SettingsStore::for_home(dir);
    monkeybyte::SettingsStore prompts((dir / "mb.db").string());

    auto add = [&](const std::string& pname, const std::string& host,
                   const std::string& plugin, const std::string& model_id) {
        UiProvider p;
        p.name = pname;
        p.host = host;
        p.api_key = "k";
        p.plugin = plugin;
        mb::save_provider(registry, p);
        UiModel m;
        m.name = pname;
        m.provider = pname;
        m.model_id = model_id;
        mb::save_model(registry, m);
    };
    add("mm", "https://api.minimax.io", "", "MiniMax-M3");   // host auto-detect
    add("kimi", "https://api.moonshot.ai", "", "kimi-k3");   // host auto-detect
    add("kimi31", "https://api.moonshot.ai", "", "kimi-k3.1-pro");
    add("kimi31hs", "https://api.moonshot.ai", "", "kimi-k3.1-pro-highspeed");
    add("code31", "https://api.kimi.com/coding/v1", "", "k3.1-pro");
    add("code31hs", "https://api.kimi.ai/coding/v1", "", "k3.1-pro-highspeed");
    add("plain", "https://api.example.com", "", "gpt-x");    // openai-compatible

    const auto mm = mb::supported_thinking_levels(registry, prompts, "mm - MiniMax-M3");
    check(mm.size() == 1 && mm[0] == ThinkingLevel::None, "MiniMax-M3 offers None");

    const auto k3 = mb::supported_thinking_levels(registry, prompts, "kimi - kimi-k3");
    check(k3.size() == 3 && k3[0] == ThinkingLevel::Low &&
              k3[1] == ThinkingLevel::High && k3[2] == ThinkingLevel::Max,
          "kimi-k3 offers Low/High/Max");

    // §K3.1: the chat-header dropdown receives Low/High/Max for both
    // platform and Kimi Code IDs through the unchanged ModelBridge path.
    for (const char* display : {
             "kimi31 - kimi-k3.1-pro",
             "kimi31hs - kimi-k3.1-pro-highspeed",
             "code31 - k3.1-pro",
             "code31hs - k3.1-pro-highspeed",
         }) {
        const auto levels = mb::supported_thinking_levels(registry, prompts, display);
        check(levels.size() == 3 && levels[0] == ThinkingLevel::Low &&
                  levels[1] == ThinkingLevel::High && levels[2] == ThinkingLevel::Max,
              std::string(display) + " offers Low/High/Max");
    }

    check(mb::supported_thinking_levels(registry, prompts, "plain - gpt-x").empty(),
          "openai-compatible model has no levels");
    check(mb::supported_thinking_levels(registry, prompts, "nope - nope").empty(),
          "unknown display yields empty");

    // Explicit plugin hint wins over host auto-detect (daedalus plugin_for_row).
    add("hint", "https://api.example.com", "minimax", "MiniMax-M3");
    check(mb::supported_thinking_levels(registry, prompts, "hint - MiniMax-M3").size() == 1,
          "explicit plugin hint honored");

    fs::remove_all(dir);
}

// ── WorkspaceTree logic (§4.2) ──────────────────────────────────────
void test_workspace_tree_logic() {
    std::cout << "WorkspaceTree (logic)\n";
    std::unordered_set<std::string> expanded = {"/ws/src", "/ws/docs"};
    check(monkeybyte::WorkspaceTree::is_expanded(expanded, "/ws/src"),
          "expanded path found");
    check(!monkeybyte::WorkspaceTree::is_expanded(expanded, "/ws/other"),
          "unexpanded path not found");
}

// ── SearchService (§4.3) ────────────────────────────────────────────
void test_search_service() {
    std::cout << "SearchService\n";
    const auto dir = make_temp_dir("search");
    {
        std::ofstream(dir / "a.txt") << "hello world\nsecond line\n";
        std::ofstream(dir / "b.txt") << "nothing here\n";
        std::ofstream(dir / "utf8.txt") << "hello wörld\n"; // UTF-8 stays text
        fs::create_directories(dir / ".hidden");
        std::ofstream(dir / ".hidden" / "c.txt") << "hello hidden\n";
        // fake binary: contains the needle plus a NUL (like a.out would)
        const char bin[] = {'H', 'E', 'L', 'L', 'O', '\0', '\x01', '\x02'};
        std::ofstream(dir / "bin.dat", std::ios::binary).write(bin, sizeof(bin));
        // excludable entries: build dir + object file
        fs::create_directories(dir / "build");
        std::ofstream(dir / "build" / "x.txt") << "hello from build\n";
        std::ofstream(dir / "obj.o") << "hello object\n";
    }
    // no excludes: everything text matches, incl. build/ and obj.o
    const auto hits = monkeybyte::SearchService::search(dir, "HELLO");
    check(hits.size() == 4, "no excludes: text + UTF-8 + build + obj.o");
    check(std::none_of(hits.begin(), hits.end(),
                       [](const auto& h) {
                           return h.path.find("bin.dat") != std::string::npos;
                       }),
          "binary file with needle not matched");

    // default excludes: build dir and *.o skipped
    const auto excl = monkeybyte::SearchService::search(
        dir, "HELLO", monkeybyte::SettingsStore::default_search_excludes());
    check(excl.size() == 2, "default excludes skip build dir and *.o");
    check(std::any_of(excl.begin(), excl.end(),
                      [](const auto& h) {
                          return h.path.find("a.txt") != std::string::npos;
                      }),
          "correct file matched");
    check(std::any_of(excl.begin(), excl.end(),
                      [](const auto& h) {
                          return h.path.find("utf8.txt") != std::string::npos;
                      }),
          "UTF-8 file matched");
    // relative-path pattern form
    check(monkeybyte::SearchService::search(dir, "HELLO", {"build/*"}).size() == 3,
          "relative-path exclude skips build subtree");
    check(monkeybyte::SearchService::search(dir, "").empty(), "empty query empty");
    fs::remove_all(dir);
}

// ── Markdown (§7.4) ─────────────────────────────────────────────────
void test_markdown() {
    std::cout << "Markdown\n";
    using monkeybyte::Markdown;

    check(Markdown::to_pango("a & b <c>").find("&amp;") != std::string::npos,
          "escapes ampersand");
    check(Markdown::to_pango("a & b").find("&") != std::string::npos,
          "plain text passes");
    check(Markdown::to_pango("**bold**").find("<b>bold</b>") != std::string::npos,
          "bold");
    check(Markdown::to_pango("*it*").find("<i>it</i>") != std::string::npos,
          "italic");
    check(Markdown::to_pango("**not italic**").find("<i>") == std::string::npos,
          "bold not confused with italic");
    check(Markdown::to_pango("use `x.y()`").find("monospace") != std::string::npos,
          "inline code");
    const auto fenced = Markdown::to_pango("```cpp\nint x = 1;\n```");
    check(fenced.find("monospace") != std::string::npos, "fenced code block");
    check(fenced.find("#cba6f7") != std::string::npos, "keyword highlighted");
    check(Markdown::to_pango("- one\n- two").find("• one") != std::string::npos,
          "list bullet");
    // Streaming deltas can end mid-fence, before the language line's
    // newline arrives — must not throw (std::out_of_range → GLib abort).
    bool threw = false;
    try {
        Markdown::to_pango("```");
        Markdown::to_pango("```cpp");
        Markdown::to_pango("some text\n```python");
        Markdown::to_pango("```cpp\nint x = 1;\n```\nmore\n```");
    } catch (const std::exception&) {
        threw = true;
    }
    check(!threw, "partial trailing fence does not throw");
}

// ── MarkdownToHtml (§6 preview tabs) ────────────────────────────────
void test_markdown_to_html() {
    std::cout << "MarkdownToHtml\n";
    using monkeybyte::MarkdownToHtml;

    check(MarkdownToHtml::to_body("# Title").find("<h1>Title</h1>") != std::string::npos,
          "heading becomes h1");
    check(MarkdownToHtml::to_body("**bold**").find("<strong>bold</strong>") != std::string::npos,
          "bold becomes strong");
    check(MarkdownToHtml::to_body("```cpp\nint x = 1;\n```").find("<pre>") != std::string::npos,
          "fenced code block becomes pre");
    check(MarkdownToHtml::to_body("- one\n- two").find("<li>one</li>") != std::string::npos,
          "list becomes ul/li");
    check(MarkdownToHtml::to_body("| a |\n| - |\n| b |").find("<table>") != std::string::npos,
          "table becomes table");
    check(MarkdownToHtml::to_body("<script>alert(1)</script>").find("<script>") == std::string::npos,
          "raw HTML is dropped (safe mode)");
    const auto dark = MarkdownToHtml::to_document("# T", /*dark=*/true);
    check(dark.find("<!DOCTYPE html>") != std::string::npos &&
              dark.find("<h1>") != std::string::npos,
          "document wraps body in full html");
    check(dark.find("#1e1e2e") != std::string::npos, "dark palette (Mocha)");
    check(MarkdownToHtml::to_document("# T", /*dark=*/false).find("#eff1f5") != std::string::npos,
          "light palette (Latte)");
}

// ── User commands (§7.8) ────────────────────────────────────────────
void test_user_commands() {
    std::cout << "UserCommands\n";
    using monkeybyte::parse_command;
    using monkeybyte::looks_like_command;
    using monkeybyte::user_commands;

    check(user_commands().size() >= 8, "command table populated");
    check(parse_command("/init").value_or("") == "init", "parses /init");
    check(parse_command("  /compress  ").value_or("") == "compress",
          "trims whitespace");
    check(parse_command("/help").value_or("") == "help", "parses /help");
    check(!parse_command("hello world"), "plain text is not a command");
    check(!parse_command("/"), "bare slash is not a command");
    check(!parse_command("/bogus"), "unknown command rejected");
    check(!parse_command("/init please"), "commands take no arguments");
    check(looks_like_command("/bogus"), "unknown slash still looks like command");
    check(!looks_like_command("hello/world"), "slash mid-text is not a command");
}

// ── SkillDiscovery (§4.5 Agent tab) ──────────────────────────────────
void test_skill_discovery() {
    std::cout << "SkillDiscovery\n";
    const auto dir = make_temp_dir("skills");

    fs::create_directories(dir / "firefox");
    {
        std::ofstream f(dir / "firefox" / "SKILL.md");
        f << "---\nname: firefox\ndescription: \"Drive the browser\"\n---\n\nBody.\n";
    }
    fs::create_directories(dir / "minimal");
    {
        std::ofstream f(dir / "minimal" / "SKILL.md");
        f << "---\nname: minimal\n---\n";
    }
    fs::create_directories(dir / "not-a-skill"); // no SKILL.md — skipped

    const auto skills = monkeybyte::discover_skills(dir);
    check(skills.size() == 2, "two skills discovered, non-skill dir skipped");
    const auto find = [&](const std::string& n) -> const monkeybyte::SkillEntry* {
        const auto it = std::find_if(
            skills.begin(), skills.end(),
            [&](const monkeybyte::SkillEntry& s) { return s.name == n; });
        return it != skills.end() ? &*it : nullptr;
    };
    const auto* ff = find("firefox");
    check(ff && ff->description == "Drive the browser",
          "front-matter name + quoted description parsed");
    const auto* min = find("minimal");
    check(min && min->description.empty(), "missing description tolerated");

    fs::create_directories(dir / "fallback");
    {
        std::ofstream f(dir / "fallback" / "SKILL.md");
        f << "no front matter here\n";
    }
    const auto skills2 = monkeybyte::discover_skills(dir);
    const bool fell_back = std::any_of(
        skills2.begin(), skills2.end(),
        [](const monkeybyte::SkillEntry& s) { return s.name == "fallback"; });
    check(fell_back, "no front matter falls back to directory name");

    check(monkeybyte::discover_skills(dir / "does-not-exist").empty(),
          "missing root yields empty list");

    fs::remove_all(dir);
}

// ── File icons (§4.2 symbolic set) ──────────────────────────────────
void test_file_icons() {
    std::cout << "FileIcons\n";
    using monkeybyte::file_icon_name;

    check(file_icon_name("src", true) == "mb-folder-symbolic", "folder closed");
    // cppcheck-suppress knownConditionTrueFalse // const-evaluated by cppcheck; regression value is intentional
    check(file_icon_name("src", true, true) == "mb-folder-open-symbolic",
          "folder expanded");
    check(file_icon_name("main.cpp", false) == "mb-file-code-symbolic",
          "code by extension");
    check(file_icon_name("MAIN.CPP", false) == "mb-file-code-symbolic",
          "extension match is case-insensitive");
    check(file_icon_name("run.sh", false) == "mb-file-script-symbolic", "script");
    check(file_icon_name("logo.png", false) == "mb-file-image-symbolic", "image");
    check(file_icon_name("deps.tar.gz", false) == "mb-file-archive-symbolic",
          "archive by last extension");
    check(file_icon_name("CMakeLists.txt", false) == "mb-file-config-symbolic",
          "exact filename beats extension");
    check(file_icon_name("README.md", false) == "mb-file-doc-symbolic", "readme");
    check(file_icon_name("package.json", false) == "mb-file-data-symbolic",
          "package manifest");
    check(file_icon_name(".env", false) == "mb-file-lock-symbolic", "dotfile env");
    check(file_icon_name("mystery.xyz", false) == "mb-file-symbolic",
          "unknown falls back to generic file");
    check(file_icon_name("noext", false) == "mb-file-symbolic",
          "extensionless falls back to generic file");
}

} // namespace

// ── HtmlToText (§6 web_fetch) ───────────────────────────────────────
void test_html_to_text() {
    std::cout << "HtmlToText\n";
    using monkeybyte::HtmlToText;

    check(HtmlToText::to_text("<p>Hello <b>world</b></p>") == "Hello world",
          "inline tags stripped");
    check(HtmlToText::to_text("<p>one</p><p>two</p>") == "one\ntwo",
          "block tags produce line breaks");
    check(HtmlToText::to_text(
              "<div><script>var x = 1;</script><style>.a{}</style>keep</div>") ==
              "keep",
          "script/style content dropped");
    check(HtmlToText::to_text("fish &amp; chips &lt;ok&gt; &quot;q&quot; &#39;"
                              " &nbsp;x") == "fish & chips <ok> \"q\" ' x",
          "entities decoded");
    check(HtmlToText::to_text("<p>a</p>\n\n\n\n<p>b</p>") == "a\n\nb",
          "blank-line runs collapse to one");
    check(HtmlToText::to_text("plain text") == "plain text",
          "plain text passes through");
    check(HtmlToText::to_text("<!-- hidden -->visible") == "visible",
          "comments dropped");
}



// ── FileTemplates (§4.2 "Add File from Template…") ──────────────────
void test_file_templates() {
    std::cout << "FileTemplates\n";
    using monkeybyte::include_guard_name;
    using monkeybyte::file_templates;

    check(include_guard_name("foo-bar.h") == "FOO_BAR_H", "guard: dash and dot fold");
    check(include_guard_name("My File.hpp") == "MY_FILE_HPP", "guard: uppercase + space");
    check(include_guard_name("") == "", "guard: empty");

    const auto& tmpls = file_templates();
    check(!tmpls.empty(), "templates registered");
    bool all_labeled = true, all_nonempty = true;
    for (const auto& t : tmpls) {
        if (t.label.empty() || t.default_name.empty()) all_labeled = false;
        if (t.make_content(t.default_name).empty()) all_nonempty = false;
    }
    check(all_labeled, "all templates have label + default name");
    check(all_nonempty, "all templates produce content");

    const auto header = tmpls.front().make_content("thing.h");
    check(header.find("#ifndef THING_H") != std::string::npos &&
              header.find("#endif // THING_H") != std::string::npos,
          "header template guard matches filename");
}


void test_project_types() {
    std::cout << "ProjectTypes\n";
    using monkeybyte::ProjectTypeLibrary;

    // temp dir fixture
    const std::string base = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
        + "/mb_ptypes_test";
    std::filesystem::remove_all(base);
    std::filesystem::create_directories(base + "/types/film.templates");
    std::filesystem::create_directories(base + "/ws_cpp");
    std::filesystem::create_directories(base + "/ws_film");
    std::filesystem::create_directories(base + "/ws_blank");

    auto write = [](const std::string& p, const std::string& text) {
        std::ofstream f(p, std::ios::trunc);
        f << text;
    };

    write(base + "/types/generic.md",
          "---\nid: generic\nname: Generic\n---\n\nGeneric prompt body.\n");
    write(base + "/types/cpp.md",
          "---\nid: cpp\nname: C++\ndescription: C++ project\n"
          "detect: CMakeLists.txt, *.cpp\n---\n\nC++ prompt body.\n");
    write(base + "/types/film.md",
          "---\nid: film\nname: Filmmaking\ndetect: *.fountain\n---\n\nFilm prompt.\n");
    write(base + "/types/bare.md", "No front matter here, just a prompt.\n");
    write(base + "/types/film.templates/script.fountain", "Title: T\n");

    ProjectTypeLibrary lib(base + "/types");

    // parse + scan
    check(lib.list().size() == 4, "scan: 4 types loaded (sorted)");
    check(lib.list().front().id == "bare", "scan: sorted by id");
    const auto* cpp = lib.find("cpp");
    check(cpp != nullptr, "find: cpp present");
    check(cpp && cpp->name == "C++" && cpp->description == "C++ project",
          "parse: name + description");
    check(cpp && cpp->detect_patterns.size() == 2 &&
              cpp->detect_patterns[0] == "CMakeLists.txt" &&
              cpp->detect_patterns[1] == "*.cpp",
          "parse: detect patterns split on comma");
    check(cpp && cpp->system_prompt == "C++ prompt body.", "parse: body is trimmed prompt");
    const auto* film = lib.find("film");
    check(film && !film->templates_dir.empty() &&
              film->templates_dir.find("film.templates") != std::string::npos,
          "parse: templates dir derived from id");
    const auto* bare = lib.find("bare");
    check(bare && bare->system_prompt.find("No front matter") == 0,
          "parse: no front matter -> whole file is prompt, id from filename");

    // pattern matching
    check(ProjectTypeLibrary::pattern_matches("*.fountain", "scene1.fountain"),
          "pattern: suffix glob matches");
    check(!ProjectTypeLibrary::pattern_matches("*.fountain", "scene1.md"),
          "pattern: suffix glob rejects other ext");
    check(ProjectTypeLibrary::pattern_matches("CMakeLists.txt", "CMakeLists.txt"),
          "pattern: exact filename");
    check(!ProjectTypeLibrary::pattern_matches("CMakeLists.txt", "cmakelists.txt"),
          "pattern: exact is case-sensitive");

    // heuristics
    write(base + "/ws_cpp/CMakeLists.txt", "cmake_minimum_required(VERSION 3.20)\n");
    write(base + "/ws_film/scene1.fountain", "INT. X - DAY\n");
    check(lib.detect(base + "/ws_cpp") == cpp, "detect: CMakeLists.txt -> cpp");
    check(lib.detect(base + "/ws_film") == film, "detect: *.fountain -> film");
    check(lib.detect(base + "/ws_blank") == lib.find("generic"),
          "detect: empty dir -> generic");

    // stamp read/write
    check(!ProjectTypeLibrary::read_stamp(base + "/ws_blank").has_value(),
          "stamp: none in workspace without AGENTS.md");
    std::string err;
    check(ProjectTypeLibrary::write_stamp(base + "/ws_blank", "film", &err),
          "stamp: write creates AGENTS.md (" + err + ")");
    const auto s1 = ProjectTypeLibrary::read_stamp(base + "/ws_blank");
    check(s1 && *s1 == "film", "stamp: read back film");
    check(lib.detect(base + "/ws_blank") == film, "detect: stamp beats heuristics");
    // replace stamp on existing file
    check(ProjectTypeLibrary::write_stamp(base + "/ws_blank", "cpp", &err),
          "stamp: rewrite ok");
    const auto s2 = ProjectTypeLibrary::read_stamp(base + "/ws_blank");
    check(s2 && *s2 == "cpp", "stamp: replaced with cpp");
    // existing AGENTS.md without stamp gets one prepended
    write(base + "/ws_cpp/AGENTS.md", "# My Project\n\nNotes.\n");
    check(ProjectTypeLibrary::write_stamp(base + "/ws_cpp", "cpp", &err), "stamp: prepend ok");
    std::ifstream in(base + "/ws_cpp/AGENTS.md");
    std::string first;
    std::getline(in, first);
    check(first == "<!-- monkeybyte:type: cpp -->", "stamp: first line of existing file");
    const auto s3 = ProjectTypeLibrary::read_stamp(base + "/ws_cpp");
    check(s3 && *s3 == "cpp", "stamp: read from prepended file");

    // update_prompt: body replaced, front matter preserved
    const auto* film2 = lib.find("film");
    check(film2 != nullptr, "update_prompt: film present");
    check(ProjectTypeLibrary::update_prompt(*film2, "New film prompt.", nullptr),
          "update_prompt: write ok");
    const auto reparsed = ProjectTypeLibrary::parse_file(base + "/types/film.md");
    check(reparsed.system_prompt == "New film prompt.",
          "update_prompt: body replaced");
    check(reparsed.name == "Filmmaking" && reparsed.detect_patterns.size() == 1,
          "update_prompt: front matter preserved");
    // no-front-matter file: whole file becomes the new prompt
    check(ProjectTypeLibrary::update_prompt(*lib.find("bare"), "Bare v2.", nullptr),
          "update_prompt: bare file write ok");
    check(ProjectTypeLibrary::parse_file(base + "/types/bare.md").system_prompt == "Bare v2.",
          "update_prompt: bare file body replaced");

    // scaffold_workspace: dir + stamped AGENTS.md + templates copied
    const std::string ws_new = base + "/new_film_ws";
    check(ProjectTypeLibrary::scaffold_workspace(ws_new, film2, nullptr),
          "scaffold: creates film workspace");
    check(std::filesystem::exists(ws_new + "/AGENTS.md"), "scaffold: AGENTS.md written");
    check(std::filesystem::exists(ws_new + "/script.fountain"),
          "scaffold: template copied");
    const auto s4 = ProjectTypeLibrary::read_stamp(ws_new);
    check(s4 && *s4 == "film", "scaffold: stamp readable");
    // idempotent: rerun keeps existing files (AGENTS.md modified by user)
    { std::ofstream f(ws_new + "/script.fountain"); f << "USER EDITS\n"; }
    check(ProjectTypeLibrary::scaffold_workspace(ws_new, film2, nullptr),
          "scaffold: rerun ok");
    std::ifstream chk(ws_new + "/script.fountain");
    std::string first_line;
    std::getline(chk, first_line);
    check(first_line == "USER EDITS", "scaffold: never overwrites existing files");
    // nullptr type = plain folder, no stamp
    const std::string ws_plain = base + "/plain";
    check(ProjectTypeLibrary::scaffold_workspace(ws_plain, nullptr, nullptr),
          "scaffold: plain folder ok");
    check(!std::filesystem::exists(ws_plain + "/AGENTS.md"),
          "scaffold: plain folder gets no AGENTS.md");

    std::filesystem::remove_all(base);
}

void test_fountain_zoom() {
    std::cout << "test_fountain_zoom\n";
    using monkeybyte::HtmlView;
    const double eps = 1e-9;
    // Standard 1080p desktop at 100%: no zoom needed.
    check(std::abs(HtmlView::fountain_default_zoom(92, 1.0, 1.0) - 1.0) < eps,
          "zoom: 1080p @100% -> 1.0");
    // 4K 27\" (~163 DPI) at 100% desktop scaling: print-styled text is
    // physically tiny -> zoom up ~1.7x.
    check(std::abs(HtmlView::fountain_default_zoom(163, 1.0, 1.0) - 1.7) < eps,
          "zoom: 4K @100% -> 1.7");
    // Same 4K panel at 200% desktop scaling: the compositor already
    // doubles everything -> stay at 1.0.
    check(std::abs(HtmlView::fountain_default_zoom(163, 2.0, 1.0) - 1.0) < eps,
          "zoom: 4K @200% -> 1.0");
    // GNOME "Large Text" (1.25) raises the default proportionally.
    check(std::abs(HtmlView::fountain_default_zoom(163, 2.0, 1.25) - 1.05) < eps,
          "zoom: text-scaling-factor applied");
    // Unknown physical size (dpi <= 0) assumes standard density.
    check(std::abs(HtmlView::fountain_default_zoom(0, 1.0, 1.0) - 1.0) < eps,
          "zoom: unknown dpi -> 1.0");
    // Clamped to [1.0, 3.0].
    check(std::abs(HtmlView::fountain_default_zoom(600, 1.0, 1.0) - 3.0) < eps,
          "zoom: clamped at 3.0");
    check(std::abs(HtmlView::fountain_default_zoom(30, 1.0, 1.0) - 1.0) < eps,
          "zoom: clamped at 1.0");
    // Snapped to 5% steps.
    const double z = HtmlView::fountain_default_zoom(150, 1.0, 1.0);
    check(std::abs(z * 20.0 - std::round(z * 20.0)) < 1e-6,
          "zoom: snapped to 5% steps");
}

void test_scroll_pin_kinetic_cancel() {
    auto app = Gtk::Application::create();
    if (!Gdk::Display::get_default()) {
        std::cout << "  skip: scroll pin kinetic cancellation (no display)\n";
        return;
    }
    Gtk::Window window;
    auto* scroller = Gtk::make_managed<Gtk::ScrolledWindow>();
    auto* content = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
    content->set_size_request(400, 2000);
    scroller->set_size_request(400, 300);
    scroller->set_child(*content);
    window.set_child(*scroller);
    window.show();

    auto pump = [](int milliseconds) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(milliseconds);
        while (std::chrono::steady_clock::now() < deadline) {
            while (g_main_context_iteration(nullptr, FALSE)) {}
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    pump(100);
    auto adj = scroller->get_vadjustment();
    adj->set_value(adj->get_upper() - adj->get_page_size());
    pump(30);
    const double initial = adj->get_value();
    check(initial > 0, "scroll pin: tall content starts above zero");

    // Drive GTK's own scroll/deceleration controllers, not a synthetic
    // adjustment write. This reproduces the production failure where the
    // deceleration tick moves the viewport again after a forced pin.
    auto* controllers = gtk_widget_observe_controllers(GTK_WIDGET(scroller->gobj()));
    const guint count = g_list_model_get_n_items(controllers);
    for (guint i = 0; i < count; ++i) {
        auto* item = g_list_model_get_item(controllers, i);
        auto* controller = GTK_EVENT_CONTROLLER(item);
        if (GTK_IS_EVENT_CONTROLLER_SCROLL(controller) &&
            gtk_event_controller_get_propagation_phase(controller) != GTK_PHASE_CAPTURE) {
            g_signal_emit_by_name(controller, "scroll-begin");
            gboolean handled = false;
            g_signal_emit_by_name(controller, "scroll", 0.0, -10.0, &handled);
        }
        g_object_unref(item);
    }
    for (guint i = 0; i < count; ++i) {
        auto* item = g_list_model_get_item(controllers, i);
        auto* controller = GTK_EVENT_CONTROLLER(item);
        if (GTK_IS_EVENT_CONTROLLER_SCROLL(controller) &&
            gtk_event_controller_get_propagation_phase(controller) == GTK_PHASE_CAPTURE)
            g_signal_emit_by_name(controller, "decelerate", 0.0, -800.0);
        g_object_unref(item);
    }
    pump(20);
    check(adj->get_value() < initial - 1.0,
          "scroll pin: GTK deceleration is active before cancellation");

    monkeybyte::cancel_scroller_deceleration(*scroller);
    const double target = adj->get_upper() - adj->get_page_size();
    adj->set_value(target);
    pump(300);
    check(std::abs(adj->get_value() - target) < 0.01,
          "scroll pin: deceleration cannot override the pinned bottom");
    check(scroller->get_kinetic_scrolling(),
          "scroll pin: kinetic scrolling setting is restored");

    window.hide();
    pump(20);
}

int main() {
    // giomm object wrapping (FileKind content-type queries) needs glibmm
    // initialized; in the app Gtk::Application does this for us.
    Glib::init();
    Gio::init(); // giomm wrap table (GFileInfo, GAppInfo, …)

    test_file_service();
    test_file_kind();
    test_file_templates();
    test_project_types();
    test_chimpflow_settings_and_client();
    test_chimpflow_http_contract();
    test_wav_contract();
    test_speech_stream();
    test_audio_normalisation();
    test_chimpflow_voice_service();
    test_settings_store();
    test_mcp_bridge();
    test_mcp_json_import();
    test_model_bridge();
    test_model_bridge_migration();
    test_model_bridge_thinking_levels();
    test_skill_discovery();
    test_workspace_tree_logic();
    test_search_service();
    test_markdown();
    test_markdown_to_html();
    test_file_icons();
    test_user_commands();
    test_html_to_text();
    test_fountain_zoom();
    test_scroll_pin_kinetic_cancel();

    std::cout << (failures == 0 ? "\nALL TESTS PASSED\n"
                                : "\n" + std::to_string(failures) + " FAILURES\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
