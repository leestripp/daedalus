// SPDX-License-Identifier: BSL-1.0
// src/api/providers/MoonshotPlugin.cpp

#include "api/providers/MoonshotPlugin.h"

#include <algorithm>
#include <cctype>

namespace daedalus {
namespace {

// Static Kimi model table, from the platform model/parameter references
// (https://platform.kimi.ai/docs/models, 2026-10-25):
//   kimi-k3.1-pro(-highspeed)  1,048,576-token context, vision, thinking
//   kimi-k3                    1M context
//   kimi-k2.7-code(-highspeed) 256K context, temperature fixed at 1.0
//   kimi-k2.6 / kimi-k2.5      256K context, thinking + non-thinking
//   moonshot-v1-*              8K / 32K / 128K context
// The deprecated kimi-k2 preview/thinking series is deliberately absent.
//
// The Kimi Code subscription endpoints (api.kimi.com / api.kimi.ai) use
// their own model IDs — same underlying models, different names:
//   k3.1-pro(-highspeed)        Kimi K3.1 Pro (up to 1,048,576 tokens)
//   k3 / k3-256k                Kimi K3 (1M / 256K context)
//   kimi-for-coding(-highspeed) Kimi K2.8 Preview (1M context; §K2.8 below)
// §K2.8 (2026-09-30): the kimi-for-coding IDs route to Kimi K2.8 Preview
// server-side. K2.8 adds K3-style low/high/max effort, so the payload and
// capability logic below treats kimi-for-coding like the K3 ids.
// §K3.1 (2026-10-25): K3.1 Pro supports the same low/high/max levels and
// fixed temperature=1.0 as K3. Its exact context window is 1,048,576 tokens
// (platform pricing/reference docs); Kimi Code plan tiers can cap it lower.
// NOTE: the platform documents the "temperature fixed at 1.0" rule for
// the kimi-k2.7-code IDs; the Kimi Code endpoint enforces the same rule
// for kimi-for-coding (confirmed against the live server 2026-07-31), so
// clamp_temperature() covers both.
const ProviderModelInfo kModels[] = {
    {"kimi-k3.1-pro",              1048576, 1.0, true,  true},
    {"kimi-k3.1-pro-highspeed",    1048576, 1.0, true,  true},
    {"k3.1-pro",                   1048576, 1.0, true,  true},
    {"k3.1-pro-highspeed",         1048576, 1.0, true,  true},
    {"kimi-k3",                     1000000, 1.0, true,  true},
    {"k3",                          1000000, 1.0, true,  true},
    {"k3-256k",                      262144, 1.0, true,  true},
    {"kimi-k2.7-code",               262144, 1.0, true,  true},
    {"kimi-k2.7-code-highspeed",     262144, 1.0, true,  true},
    {"kimi-for-coding",             1000000, 1.0, true,  true},
    {"kimi-for-coding-highspeed",   1000000, 1.0, true,  true},
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

bool istarts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

bool is_k31(std::string_view model_id) {
    return istarts_with(model_id, "kimi-k3.1-") ||
           istarts_with(model_id, "k3.1-");
}

bool is_k3_family(std::string_view model_id) {
    return istarts_with(model_id, "kimi-k3") ||
           iequals(model_id, "k3") || istarts_with(model_id, "k3-") ||
           is_k31(model_id);
}

bool has_k3_style_effort(std::string_view model_id) {
    return is_k3_family(model_id) || icontains(model_id, "kimi-for-coding");
}

} // namespace

bool MoonshotPlugin::matches(std::string_view host) const {
    // Covers api.moonshot.ai, api.moonshot.cn, and the Kimi Code
    // subscription endpoints api.kimi.com / api.kimi.ai.
    return icontains(host, "moonshot") || icontains(host, "kimi");
}

double MoonshotPlugin::clamp_temperature(std::string_view model_id, double t) const {
    // kimi-k2.7-code(-highspeed) uses a fixed temperature of 1.0 — any
    // other value is a hard API error (platform docs, "Parameters
    // Differences in Request Body"). The Kimi Code endpoint's
    // kimi-for-coding IDs (K2.8 Preview since §K2.8) and its
    // k3/k3-256k IDs enforce the same rule (confirmed empirically
    // against the live server 2026-07-31: "invalid temperature: only
    // 1 is allowed for this model"); the clamp is kept for K2.8 since
    // a rejected temperature is a hard error. Scoped to the exact Kimi
    // Code k3 IDs so the platform's original kimi-k3 behavior is left
    // unchanged. §K3.1: the current platform parameter reference explicitly
    // gives K3.1 Pro and Highspeed the same fixed temperature=1.0 constraint.
    if (icontains(model_id, "k2.7-code") ||
        icontains(model_id, "kimi-for-coding") ||
        iequals(model_id, "k3") ||
        istarts_with(model_id, "k3-") || is_k31(model_id)) {
        return 1.0;
    }
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

    // §ThinkingLevel (2026-08-01): host-selected thinking level. Mapping
    // (https://platform.kimi.ai/docs/guide/use-thinking-models):
    //   - K3/K3.1 ids: reasoning effort low/high/max. K3.1 uses top-level
    //     reasoning_effort on BOTH the platform and Kimi Code endpoints
    //     (current official K3.1 + Kimi Code integration docs). Existing K3
    //     IDs retain the legacy thinking.effort dialect on api.kimi.com
    //     (empirically verified, oh-my-pi #5893) for compatibility. The
    //     platform must NOT receive a `thinking` object. None is ignored —
    //     K3/K3.1 always thinks.
    //   - kimi-k2.5 / kimi-k2.6: on/off only (thinking.type). Effort
    //     levels are ignored.
    //   - kimi-k2.7-code: always thinks, no control — never send
    //     anything (passing disabled is a hard error).
    //   - kimi-for-coding: §K2.8 (2026-09-30) — now Kimi K2.8 Preview,
    //     which takes the same low/high/max effort as K3 (same host
    //     split: thinking.effort on api.kimi.com, reasoning_effort on
    //     the platform). None is still ignored.
    if (ctx.thinking_level != ThinkingLevel::Default) {
        const std::string& id = ctx.model_id;
        const bool has_effort = has_k3_style_effort(id);
        const bool is_k25_k26 = icontains(id, "k2.5") || icontains(id, "k2.6");
        if (has_effort &&
            (ctx.thinking_level == ThinkingLevel::Low ||
             ctx.thinking_level == ThinkingLevel::High ||
             ctx.thinking_level == ThinkingLevel::Max)) {
            std::string effort = to_string(ctx.thinking_level);
            for (auto& c : effort) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (!is_k31(id) && icontains(ctx.base_url, "kimi.com")) {
                payload["thinking"] = {{"type", "enabled"}, {"effort", effort}};
            } else {
                payload["reasoning_effort"] = effort;
            }
        } else if (is_k25_k26 && ctx.thinking_level == ThinkingLevel::None) {
            payload["thinking"] = {{"type", "disabled"}};
        }
    }
}

std::vector<ThinkingLevel>
MoonshotPlugin::supported_thinking_levels(std::string_view model_id) const {
    if (has_k3_style_effort(model_id)) {
        return {ThinkingLevel::Low, ThinkingLevel::High, ThinkingLevel::Max};
    }
    if (icontains(model_id, "k2.5") || icontains(model_id, "k2.6")) {
        return {ThinkingLevel::None};
    }
    return {};
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
