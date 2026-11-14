#pragma once

#include "monkeybyte/backend/SettingsStore.h"

#include <glibmm/dispatcher.h>
#include <sigc++/sigc++.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace monkeybyte {

class AudioBackend;
class ChimpFlowClient;

// Markdown to speech-friendly UTF-8 prose. No byte truncation: link labels and
// code/prose survive, HTML/fence markers and image targets do not.
std::string speech_text(const std::string& markdown);

struct VoiceCatalog {
    std::vector<std::string> models;
    std::vector<std::string> voices;
};

enum class DictationState { Idle, Recording, Transcribing };

// §CHIMPFLOW — one owner for Settings and Chat. Public methods/signals are UI
// thread affairs; network runs on jthreads and all completions return through
// a Glib::Dispatcher. No widget type appears here.
class VoiceService {
public:
    using CatalogCallback =
        std::function<void(const VoiceCatalog&, const std::string& error)>;
    using CompletionCallback = std::function<void(const std::string& error)>;

    explicit VoiceService(SettingsStore& store);
    VoiceService(SettingsStore& store, std::shared_ptr<ChimpFlowClient> client,
                 std::shared_ptr<AudioBackend> audio);
    ~VoiceService();

    VoiceService(const VoiceService&) = delete;
    VoiceService& operator=(const VoiceService&) = delete;

    void reload();
    const ChimpFlowSettings& settings() const { return settings_; }

    void discover(const ChimpFlowSettings& config, CatalogCallback callback);
    void preview(const ChimpFlowSettings& config, const std::string& voice,
                 CompletionCallback callback);
    void speak(const std::string& assistant_markdown);

    void toggle_dictation();
    void stop_all();

    DictationState dictation_state() const { return dictation_state_; }
    bool speech_active() const { return speech_active_; }
    std::size_t queued_speech() const { return speech_queue_.size(); }

    sigc::signal<void(const ChimpFlowSettings&)> signal_settings_changed() {
        return signal_settings_changed_;
    }
    sigc::signal<void(DictationState)> signal_dictation_state_changed() {
        return signal_dictation_state_changed_;
    }
    sigc::signal<void(const std::string&)> signal_dictation_text() {
        return signal_dictation_text_;
    }
    sigc::signal<void(const std::string&)> signal_error() { return signal_error_; }
    sigc::signal<void(bool)> signal_speech_active_changed() {
        return signal_speech_active_changed_;
    }

private:
    enum class SpeechKind { Assistant, Preview };
    struct SpeechRequest {
        ChimpFlowSettings config;
        std::string text;
        SpeechKind kind = SpeechKind::Assistant;
        CompletionCallback callback;
    };

    void post(std::function<void()> callback);
    void drain_posted();
    void start_next_speech();
    void finish_speech(std::uint64_t generation, const std::string& error);
    void stop_speech();
    void set_speech_active(bool active);
    void set_dictation_state(DictationState state);
    void join_workers();

    SettingsStore& store_;
    std::shared_ptr<ChimpFlowClient> client_;
    std::shared_ptr<AudioBackend> audio_;
    ChimpFlowSettings settings_;

    std::deque<SpeechRequest> speech_queue_;
    std::optional<SpeechRequest> current_speech_;
    bool speech_active_ = false;
    std::uint64_t speech_generation_ = 0;
    std::uint64_t dictation_generation_ = 0;
    std::uint64_t discovery_generation_ = 0;
    DictationState dictation_state_ = DictationState::Idle;

    std::jthread speech_worker_;
    std::jthread dictation_worker_;
    std::jthread discovery_worker_;

    Glib::Dispatcher dispatcher_;
    std::mutex posted_mutex_;
    std::deque<std::function<void()>> posted_;
    std::atomic<bool> alive_{true};

    sigc::signal<void(const ChimpFlowSettings&)> signal_settings_changed_;
    sigc::signal<void(DictationState)> signal_dictation_state_changed_;
    sigc::signal<void(const std::string&)> signal_dictation_text_;
    sigc::signal<void(const std::string&)> signal_error_;
    sigc::signal<void(bool)> signal_speech_active_changed_;
};

} // namespace monkeybyte
