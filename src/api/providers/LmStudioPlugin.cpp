// SPDX-License-Identifier: BSL-1.0
// src/api/providers/LmStudioPlugin.cpp

#include "api/providers/LmStudioPlugin.h"

namespace daedalus {

bool LmStudioPlugin::matches(std::string_view host) const {
    // Any loopback OpenAI-compatible server. LM Studio's default is
    // http://localhost:1234; matching all loopback hosts also covers
    // llama.cpp/ollama-style servers on other ports.
    return host.find("localhost") != std::string_view::npos ||
           host.find("127.0.0.1") != std::string_view::npos;
}

void LmStudioPlugin::mutate_chat_payload(nlohmann::json& payload,
                                         const ProviderChatContext&) const {
    // Deliberately does NOT call the base hook: the generic
    // stream_options.include_usage and any other provider-extension
    // fields are stripped so the request stays within the plain
    // OpenAI chat-completions shape local servers accept.
    payload.erase("stream_options");
    payload.erase("reasoning_split");
    payload.erase("prompt_cache_key");
}

} // namespace daedalus
