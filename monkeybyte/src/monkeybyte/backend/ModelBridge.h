#pragma once

#include <daedalus/types/ThinkingLevel.h>

#include <string>
#include <vector>

namespace daedalus {
class SettingsStore;
}

namespace monkeybyte {

class SettingsStore; // monkeybyte's own store (model_prompts binding)

// UI-level provider model: one shared endpoint/credential config (design
// §4.5 Providers tab). Mirrors daedalus::ProviderConfig.
struct UiProvider {
    std::string name; // unique id
    std::string host;
    std::string api_key; // may contain a ${ENV_VAR} placeholder (§4.5)
    std::string chat_endpoint = "/v1/chat/completions";
    std::string discovery_endpoint = "/v1/models";
    int timeout_seconds = 30;
    // daedalus §ProviderPlugins: registered provider-plugin name
    // ("minimax", "moonshot", "lmstudio", "openai-compatible", ...).
    // Empty = auto-detect from provider name/host.
    std::string plugin;
};

// UI-level model: references a UiProvider by name and keeps only
// model-specific settings. Mirrors daedalus::ModelConfig plus the
// monkeybyte-only extras (default flag, system-prompt binding).
struct UiModel {
    std::string name;       // settings key (unique id)
    std::string provider;   // UiProvider::name
    std::string model_id;   // actual model id sent to the API
    int max_context_size = 0;   // 0 = provider/model default
    double temperature = 0.0;
    bool is_default = false;
    long system_prompt_id = 0;  // 0 = default
};

// Bridge to the provider/model registry the agent actually uses.
//
// daedalus resolves models against its own SettingsStore at
// $XDG_DATA_HOME/daedalus/daedalus.db (providers table + models table,
// merged via SettingsStore::resolve_model), NOT from monkeybyte.db — so the
// Settings → Providers / Models tabs must read/write that store. These
// functions convert between the flat UI models above and daedalus's
// ProviderConfig / ModelConfig. The monkeybyte-only extras (default flag
// uses daedalus's kv_settings; the system-prompt binding lives in
// monkeybyte.db's model_prompts table).
// Call sites pass daedalus::SettingsStore::instance(); tests can inject
// SettingsStore::for_home(<tempdir>).
namespace model_bridge {

// "provider - model_id" — the display string used in dropdowns (§7.1).
std::string display_name(const UiModel& m);

// Resolves a ${ENV_VAR} placeholder in a stored API key to the env var's
// value (§4.5 env-key badge). Plain keys pass through unchanged; an unset
// variable resolves to "".
std::string resolve_env_key(const std::string& key);

std::vector<UiProvider> list_providers(daedalus::SettingsStore& store);

// Upsert by name. New providers default to the OpenAI-compatible endpoints.
void save_provider(daedalus::SettingsStore& store, const UiProvider& p);

// Deletes the provider and every model that references it (daedalus
// cascades). System-prompt bindings of those models are cleaned from
// `prompts`.
void remove_provider(daedalus::SettingsStore& store, SettingsStore& prompts,
                     const std::string& name);

std::vector<UiModel> list_models(daedalus::SettingsStore& store,
                                 const SettingsStore& prompts);

void save_model(daedalus::SettingsStore& store, const UiModel& m);

void remove_model(daedalus::SettingsStore& store, SettingsStore& prompts,
                  const std::string& name);

// The default model is daedalus's kv_settings "default_model".
void set_default_model(daedalus::SettingsStore& store, const std::string& name);

void set_model_system_prompt(SettingsStore& prompts,
                             const std::string& model_name, long prompt_id);

// Thinking levels the given model (by display string, §7.1) honors
// (daedalus §ThinkingLevel). Resolved WITHOUT building an agent: the
// model's provider plugin is looked up in daedalus::ProviderRegistry
// (explicit plugin hint first, then name/host auto-detect) and asked for
// its per-model capability list. Empty when the model/provider is unknown
// or the model has no thinking support (the setting is then ignored).
std::vector<daedalus::ThinkingLevel>
supported_thinking_levels(daedalus::SettingsStore& store,
                          const SettingsStore& prompts,
                          const std::string& model_display);

// One-time migration: legacy monkeybyte.db `models` rows (full provider
// config inline per model) → daedalus ProviderConfig + ModelConfig entries.
// Idempotent (guarded by the "models_migrated" kv key); drops the legacy
// table afterwards. No-op when there is nothing to migrate.
void migrate_legacy_models(SettingsStore& legacy, daedalus::SettingsStore& store);

} // namespace model_bridge

} // namespace monkeybyte
