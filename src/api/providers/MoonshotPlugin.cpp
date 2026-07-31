// SPDX-License-Identifier: BSL-1.0
// src/api/providers/MoonshotPlugin.cpp

#include "api/providers/MoonshotPlugin.h"

#include <algorithm>

namespace daedalus {
namespace {

// Static Kimi model table, from the platform model list
// (https://platform.moonshot.ai/docs/models, 2026-07):
//   kimi-k3                    1M context
//   kimi-k2.7-code(-highspeed) 256K context, temperature fixed at 1.0
//   kimi-k2.6 / kimi-k2.5      256K context, thinking + non-thinking
//   moonshot-v1-*              8K / 32K / 128K context
// The deprecated kimi-k2 preview/thinking series is deliberately absent.
//
// The Kimi Code subscription endpoint (https://api.kimi.com/coding/v1,
// https://www.kimi.com/code/docs/en/) uses its own model IDs — same
// underlying models, different names:
//   k3 / k3-256k                 Kimi K3 (1M / 256K context)
//   kimi-for-coding(-highspeed)  Kimi K2.7 Code (256K context)
// NOTE: the platform's "temperature fixed at 1.0" rule is documented for
// the kimi-k2.7-code platform IDs only; the Kimi Code docs state no such
// restriction for kimi-for-coding, so clamp_temperature() leaves it alone.
const ProviderModelInfo kModels[] = {
    {"kimi-k3",                     1000000, 1.0, true,  true},
    {"k3",                          1000000, 1.0, true,  true},
    {"k3-256k",                      262144, 1.0, true,  true},
    {"kimi-k2.7-code",               262144, 1.0, true,  true},
    {"kimi-k2.7-code-highspeed",     262144, 1.0, true,  true},
    {"kimi-for-coding",              262144, 1.0, true,  true},
    {"kimi-for-coding-highspeed",    262144, 1.0, true,  true},
    {"kimi-k2.6",                    262144, 0.6, true,  true},
    {"kimi-k2.5",                    262144, 0.6, true,  true},
    {"moonshot-v1-8k",                 8192, 0.3, false, false},
    {"moonshot-v1-32k",               32768, 0.3, false, false},
    {"moonshot-v1-128k",             131072, 0.3, false, false},
    {"moonshot-v1-8k-vision-preview",  8192, 0.3, true,  false},
    {"moonshot-v1-32k-vision-preview", 32768, 0.3, true,  false},
    {"moonshot-v1-128k-vision-preview",131072, 0.3, true,  false},
};

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(),
                      [](char x, char y) {
                          return std::tolower(static_cast<unsigned char>(x)) ==
                                 std::tolower(static_cast<unsigned char>(y));
                      });
}

bool icontains(std::string_view haystack, std::string_view needle) {
    if (needle.size() > haystack.size()) return false;
    return std::search(haystack.begin(), haystack.end(),
                       needle.begin(), needle.end(),
                       [](char x, char y) {
                           return std::tolower(static_cast<unsigned char>(x)) ==
                                  std::tolower(static_cast<unsigned char>(y));
                       }) != haystack.end();
}

} // namespace

bool MoonshotPlugin::matches(std::string_view host) const {
    // Covers api.moonshot.ai, api.moonshot.cn, and the Kimi Code
    // subscription endpoint api.kimi.com.
    return icontains(host, "moonshot") || icontains(host, "kimi");
}

double MoonshotPlugin::clamp_temperature(std::string_view model_id, double t) const {
    // kimi-k2.7-code(-highspeed) uses a fixed temperature of 1.0 — any
    // other value is a hard API error (platform docs, "Parameters
    // Differences in Request Body").
    if (icontains(model_id, "k2.7-code")) return 1.0;
    return t;
}

void MoonshotPlugin::mutate_chat_payload(nlohmann::json& payload,
                                         const ProviderChatContext& ctx) const {
    // Generic shaping first (stream_options.include_usage when streaming).
    OpenAiCompatiblePlugin::mutate_chat_payload(payload, ctx);

    // Server-side prompt caching: a stable per-agent key lets repeated
    // system prompts/tools hit Kimi's context cache. Empty key = opt out.
    if (!ctx.cache_key.empty()) {
        payload["prompt_cache_key"] = ctx.cache_key;
    }
}

std::vector<ProviderModelInfo> MoonshotPlugin::known_models() const {
    return {std::begin(kModels), std::end(kModels)};
}

int64_t MoonshotPlugin::context_size_for(std::string_view model_id) const {
    for (const auto& m : kModels) {
        if (iequals(m.model_id, model_id)) return m.context_size;
    }
    return 0;
}

} // namespace daedalus
