#include "monkeybyte/backend/ModelBridge.h"

#include "monkeybyte/backend/SettingsStore.h"

#include <daedalus/settings.h>
#include <daedalus/ProviderPlugin.h>
#include <daedalus/ProviderRegistry.h>
#include <daedalus/types/ProviderConfig.h>

#include <algorithm>
#include <cstdlib>
#include <memory>

namespace monkeybyte::model_bridge {

std::string display_name(const UiModel& m) {
    return m.provider + " - " + m.model_id;
}

// Resolve ${ENV_VAR} placeholders in stored API keys (§4.5 env-key badge).
std::string resolve_env_key(const std::string& key) {
    if (key.size() > 3 && key.starts_with("${") && key.back() == '}') {
        const char* v = std::getenv(key.substr(2, key.size() - 3).c_str());
        return v ? v : "";
    }
    return key;
}

std::vector<UiProvider> list_providers(daedalus::SettingsStore& store) {
    std::vector<UiProvider> out;
    for (const auto& p : store.list_providers_detailed()) {
        UiProvider u;
        u.name = p.name;
        u.host = p.host;
        u.api_key = p.api_key;
        u.chat_endpoint = p.chat_endpoint;
        u.discovery_endpoint = p.discovery_endpoint;
        u.timeout_seconds = p.timeout_seconds;
        u.plugin = p.plugin;
        out.push_back(std::move(u));
    }
    return out;
}

void save_provider(daedalus::SettingsStore& store, const UiProvider& p) {
    daedalus::ProviderConfig d;
    if (auto existing = store.get_provider(p.name)) d = *existing;
    d.name = p.name;
    d.host = p.host;
    d.api_key = p.api_key;
    d.chat_endpoint = p.chat_endpoint;
    d.discovery_endpoint = p.discovery_endpoint;
    d.timeout_seconds = p.timeout_seconds;
    d.plugin = p.plugin;
    store.upsert_provider(d);
}

void remove_provider(daedalus::SettingsStore& store, SettingsStore& prompts,
                     const std::string& name) {
    // daedalus::SettingsStore::delete_provider cascades to the provider's
    // models; drop their prompt bindings first.
    for (const auto& m : store.list_models())
        if (m.provider == name)
            prompts.set_model_system_prompt(m.name, 0);
    store.delete_provider(name);
}

std::vector<UiModel> list_models(daedalus::SettingsStore& store,
                                 const SettingsStore& prompts) {
    const std::string default_name = store.get_default_model_name();
    std::vector<UiModel> out;
    for (const auto& m : store.list_models()) {
        UiModel u;
        u.name = m.name;
        u.provider = m.provider;
        u.model_id = m.model_id;
        u.max_context_size = m.max_context_size;
        u.temperature = m.temperature;
        u.is_default = (m.name == default_name);
        u.system_prompt_id = prompts.get_model_system_prompt(m.name);
        out.push_back(std::move(u));
    }
    return out;
}

void save_model(daedalus::SettingsStore& store, const UiModel& m) {
    daedalus::ModelConfig d;
    if (auto existing = store.get_model(m.name)) d = *existing;
    d.name = m.name;
    d.provider = m.provider;
    d.model_id = m.model_id;
    d.max_context_size = m.max_context_size;
    d.temperature = m.temperature;
    store.upsert_model(d);
    if (m.is_default) store.set_default_model_name(m.name);
}

void remove_model(daedalus::SettingsStore& store, SettingsStore& prompts,
                  const std::string& name) {
    prompts.set_model_system_prompt(name, 0);
    store.delete_model(name);
    if (store.get_default_model_name() == name)
        store.set_default_model_name("");
}

void set_default_model(daedalus::SettingsStore& store, const std::string& name) {
    store.set_default_model_name(name);
}

void set_model_system_prompt(SettingsStore& prompts,
                             const std::string& model_name, long prompt_id) {
    prompts.set_model_system_prompt(model_name, prompt_id);
}

// daedalus §ThinkingLevel — per-model thinking capability without an agent.
std::vector<daedalus::ThinkingLevel>
supported_thinking_levels(daedalus::SettingsStore& store,
                          const SettingsStore& prompts,
                          const std::string& model_display) {
    const auto models = list_models(store, prompts);
    const auto m = std::find_if(models.begin(), models.end(), [&](const UiModel& u) {
        return display_name(u) == model_display;
    });
    if (m == models.end()) return {};

    const auto providers = list_providers(store);
    const auto p = std::find_if(providers.begin(), providers.end(),
                                [&](const UiProvider& u) { return u.name == m->provider; });
    if (p == providers.end()) return {};

    // Mirror daedalus's plugin_for_row: explicit hint, then name/host
    // auto-detect (resolve always falls back to openai-compatible).
    std::shared_ptr<daedalus::ProviderPlugin> plugin;
    if (!p->plugin.empty())
        plugin = daedalus::ProviderRegistry::plugin_for_name(p->plugin);
    if (!plugin)
        plugin = daedalus::ProviderRegistry::resolve(p->name, p->host);
    return plugin->supported_thinking_levels(m->model_id);
}

void migrate_legacy_models(SettingsStore& legacy, daedalus::SettingsStore& store) {
    if (legacy.get("models_migrated").has_value()) return;
    for (const auto& row : legacy.read_legacy_models()) {
        UiProvider p;
        p.name = row.provider_name;
        p.host = row.base_url;
        p.api_key = row.api_key;
        p.chat_endpoint = row.endpoint;
        if (p.chat_endpoint.empty()) p.chat_endpoint = "/v1/chat/completions";
        save_provider(store, p);

        UiModel m;
        m.name = row.provider_name;
        m.provider = row.provider_name;
        m.model_id = row.model_name;
        m.max_context_size = row.max_context;
        m.temperature = row.temperature;
        m.is_default = row.is_default;
        save_model(store, m);

        if (row.system_prompt_id > 0)
            legacy.set_model_system_prompt(row.provider_name, row.system_prompt_id);
    }
    legacy.drop_legacy_models();
    legacy.set("models_migrated", "1");
}

} // namespace monkeybyte::model_bridge
