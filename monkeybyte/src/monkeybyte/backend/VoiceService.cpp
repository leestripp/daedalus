#include "monkeybyte/backend/VoiceService.h"

#include "monkeybyte/backend/AudioEngine.h"
#include "monkeybyte/backend/ChimpFlowClient.h"
#include "monkeybyte/backend/HtmlToText.h"
#include "monkeybyte/backend/Logger.h"
#include "monkeybyte/backend/MarkdownToHtml.h"
#include "monkeybyte/backend/Wav.h"

#include <utility>

namespace monkeybyte {

namespace {
constexpr const char* kVoicePreviewText =
    "Hello from MonkeyByte. This is how your selected ChimpFlow voice sounds.";
}

std::string speech_text(const std::string& markdown) {
    return HtmlToText::to_text(MarkdownToHtml::to_body(markdown));
}

VoiceService::VoiceService(SettingsStore& store)
    : VoiceService(store, std::make_shared<ChimpFlowClient>(), make_pipewire_audio()) {}

VoiceService::VoiceService(SettingsStore& store,
                           std::shared_ptr<ChimpFlowClient> client,
                           std::shared_ptr<AudioBackend> audio)
    : store_(store), client_(std::move(client)), audio_(std::move(audio)),
      settings_(store.load_chimpflow()) {
    if (!client_ || !audio_) throw std::invalid_argument("VoiceService dependencies are required");
    dispatcher_.connect([this] { drain_posted(); });
}

VoiceService::~VoiceService() {
    alive_.store(false, std::memory_order_release);
    ++speech_generation_;
    ++dictation_generation_;
    ++discovery_generation_;
    speech_worker_.request_stop();
    dictation_worker_.request_stop();
    discovery_worker_.request_stop();
    if (dictation_state_ == DictationState::Recording) (void)audio_->stop_recording();
    audio_->stop_playback();
    join_workers();
    std::lock_guard lock(posted_mutex_);
    posted_.clear();
}

void VoiceService::join_workers() {
    if (speech_worker_.joinable()) speech_worker_.join();
    if (dictation_worker_.joinable()) dictation_worker_.join();
    if (discovery_worker_.joinable()) discovery_worker_.join();
}

void VoiceService::post(std::function<void()> callback) {
    if (!alive_.load(std::memory_order_acquire)) return;
    {
        std::lock_guard lock(posted_mutex_);
        if (!alive_.load(std::memory_order_acquire)) return;
        posted_.push_back(std::move(callback));
    }
    dispatcher_.emit();
}

void VoiceService::drain_posted() {
    while (alive_.load(std::memory_order_acquire)) {
        std::function<void()> callback;
        {
            std::lock_guard lock(posted_mutex_);
            if (posted_.empty()) break;
            callback = std::move(posted_.front());
            posted_.pop_front();
        }
        try {
            callback();
        } catch (const std::exception& e) {
            // One poison completion must not block the rest of the queue.
            Logger::instance().error(std::string("VoiceService callback failed: ") + e.what());
        } catch (...) {
            Logger::instance().error("VoiceService callback failed with an unknown exception");
        }
    }
}

void VoiceService::reload() {
    const auto previous = settings_;
    settings_ = store_.load_chimpflow();
    signal_settings_changed_.emit(settings_);
    if (settings_ != previous) stop_all();
}

void VoiceService::discover(const ChimpFlowSettings& config,
                            CatalogCallback callback) {
    const auto generation = ++discovery_generation_;
    if (discovery_worker_.joinable()) {
        discovery_worker_.request_stop();
        discovery_worker_.join();
    }
    discovery_worker_ = std::jthread(
        [this, config, generation, callback = std::move(callback)](
            std::stop_token token) mutable {
            VoiceCatalog catalog;
            std::string error;
            try {
                client_->health(config, token);
                catalog.models = client_->list_models(config, token);
                catalog.voices = client_->list_voices(config, token);
            } catch (const std::exception& e) {
                error = e.what();
            } catch (...) {
                error = "ChimpFlow discovery failed with an unknown exception";
            }
            post([this, generation, catalog = std::move(catalog), error,
                  callback = std::move(callback)]() mutable {
                if (generation != discovery_generation_) return;
                if (discovery_worker_.joinable()) discovery_worker_.join();
                callback(catalog, error);
            });
        });
}

void VoiceService::preview(const ChimpFlowSettings& config,
                           const std::string& voice,
                           CompletionCallback callback) {
    if (dictation_state_ != DictationState::Idle) {
        callback("Stop dictation before previewing a voice");
        return;
    }
    stop_speech();
    SpeechRequest request;
    request.config = config;
    request.config.voice = voice;
    request.text = kVoicePreviewText;
    request.kind = SpeechKind::Preview;
    request.callback = std::move(callback);
    speech_queue_.push_back(std::move(request));
    start_next_speech();
}

void VoiceService::speak(const std::string& assistant_markdown) {
    if (!settings_.enabled || !settings_.read_assistant) return;
    const std::string text = speech_text(assistant_markdown);
    if (text.empty()) return;
    SpeechRequest request;
    request.config = settings_;
    request.text = text;
    speech_queue_.push_back(std::move(request));
    start_next_speech();
}

void VoiceService::set_speech_active(bool active) {
    if (speech_active_ == active) return;
    speech_active_ = active;
    signal_speech_active_changed_.emit(active);
}

void VoiceService::start_next_speech() {
    if (speech_active_ || speech_queue_.empty() ||
        dictation_state_ != DictationState::Idle) {
        return;
    }
    current_speech_ = std::move(speech_queue_.front());
    speech_queue_.pop_front();
    set_speech_active(true);
    const auto generation = ++speech_generation_;
    const auto request = *current_speech_;

    speech_worker_ = std::jthread(
        [this, request, generation](std::stop_token token) mutable {
            try {
                const auto bytes = client_->synthesize(
                    request.config, request.text, request.config.voice, token);
                WavAudio clip;
                std::string error;
                if (!parse_wav(bytes, clip, error))
                    throw ChimpFlowError(0, "Invalid ChimpFlow WAV: " + error);
                post([this, generation, clip = std::move(clip)]() mutable {
                    if (generation != speech_generation_) return;
                    try {
                        audio_->play(clip.sample_rate, std::move(clip.samples),
                            [this, generation] {
                                post([this, generation] { finish_speech(generation, {}); });
                            });
                    } catch (const std::exception& e) {
                        finish_speech(generation, e.what());
                    } catch (...) {
                        finish_speech(generation, "Unknown PipeWire playback error");
                    }
                });
            } catch (const std::exception& e) {
                post([this, generation, error = std::string(e.what())] {
                    finish_speech(generation, error);
                });
            } catch (...) {
                post([this, generation] {
                    finish_speech(generation, "Unknown ChimpFlow synthesis error");
                });
            }
        });
}

void VoiceService::finish_speech(std::uint64_t generation,
                                 const std::string& error) {
    if (generation != speech_generation_ || !current_speech_) return;
    if (speech_worker_.joinable()) speech_worker_.join();
    audio_->stop_playback();

    auto completed = std::move(*current_speech_);
    current_speech_.reset();
    set_speech_active(false);

    if (completed.kind == SpeechKind::Preview) {
        if (completed.callback) completed.callback(error);
    } else if (!error.empty()) {
        signal_error_.emit(error);
    }
    // Errors are item-scoped: a failed synthesis never blocks later prose.
    start_next_speech();
}

void VoiceService::stop_speech() {
    ++speech_generation_;
    speech_queue_.clear();
    auto cancelled = std::move(current_speech_);
    current_speech_.reset();
    speech_worker_.request_stop();
    if (speech_worker_.joinable()) speech_worker_.join();
    audio_->stop_playback();
    set_speech_active(false);
    if (cancelled && cancelled->kind == SpeechKind::Preview && cancelled->callback)
        cancelled->callback("Voice preview cancelled");
}

void VoiceService::set_dictation_state(DictationState state) {
    if (dictation_state_ == state) return;
    dictation_state_ = state;
    signal_dictation_state_changed_.emit(state);
}

void VoiceService::toggle_dictation() {
    if (dictation_state_ == DictationState::Transcribing) return;
    if (dictation_state_ == DictationState::Recording) {
        std::vector<float> samples;
        try {
            samples = audio_->stop_recording();
        } catch (const std::exception& e) {
            set_dictation_state(DictationState::Idle);
            signal_error_.emit(e.what());
            start_next_speech();
            return;
        }
        if (samples.size() < 16000 / 4) {
            set_dictation_state(DictationState::Idle);
            signal_dictation_text_.emit({});
            start_next_speech();
            return;
        }

        set_dictation_state(DictationState::Transcribing);
        const auto generation = ++dictation_generation_;
        const auto config = settings_;
        dictation_worker_ = std::jthread(
            [this, config, generation,
             wav = make_wav_16k_mono(samples)](std::stop_token token) mutable {
                std::string text;
                std::string error;
                try {
                    text = client_->transcribe(config, wav, token);
                } catch (const std::exception& e) {
                    error = e.what();
                } catch (...) {
                    error = "Unknown ChimpFlow transcription error";
                }
                post([this, generation, text = std::move(text),
                      error = std::move(error)]() mutable {
                    if (generation != dictation_generation_) return;
                    if (dictation_worker_.joinable()) dictation_worker_.join();
                    set_dictation_state(DictationState::Idle);
                    if (!error.empty()) signal_error_.emit(error);
                    else signal_dictation_text_.emit(text);
                    start_next_speech();
                });
            });
        return;
    }

    if (!settings_.enabled || !settings_.dictation_enabled) {
        signal_error_.emit("Enable ChimpFlow and microphone dictation in Settings first");
        return;
    }
    if (resolved_chimpflow_api_key(settings_).empty() || settings_.stt_model.empty()) {
        signal_error_.emit("Configure a scoped ChimpFlow API key and STT model first");
        return;
    }
    stop_speech();
    try {
        audio_->start_recording();
        set_dictation_state(DictationState::Recording);
    } catch (const std::exception& e) {
        set_dictation_state(DictationState::Idle);
        signal_error_.emit(e.what());
    } catch (...) {
        set_dictation_state(DictationState::Idle);
        signal_error_.emit("Unknown PipeWire recording error");
    }
}

void VoiceService::stop_all() {
    ++speech_generation_;
    ++dictation_generation_;
    ++discovery_generation_;
    speech_queue_.clear();
    auto cancelled = std::move(current_speech_);
    current_speech_.reset();
    speech_worker_.request_stop();
    dictation_worker_.request_stop();
    discovery_worker_.request_stop();
    if (dictation_state_ == DictationState::Recording) (void)audio_->stop_recording();
    audio_->stop_playback();
    join_workers();
    set_dictation_state(DictationState::Idle);
    set_speech_active(false);
    if (cancelled && cancelled->kind == SpeechKind::Preview && cancelled->callback)
        cancelled->callback("Voice preview cancelled");
}

} // namespace monkeybyte
