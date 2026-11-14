// SPDX-License-Identifier: BSL-1.0
#include "AgentService.h"
#include "SessionCatalog.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <iostream>

#include <daedalus/settings.h>

int main() {
    const auto root = std::filesystem::temp_directory_path() / "monkeybyte-catalog-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "workspace");
    {
        monkeybyte::SessionCatalog catalog(root / "sessions.json");
        const auto first = catalog.ensure(root / "workspace");
        assert(catalog.ensure(root / "workspace").id == first.id);
        const auto second = catalog.create(root / "workspace", "Second");
        catalog.rename(second.id, "Renamed");
        catalog.set_model(second.id, "test-model");
        assert(catalog.active()->id == second.id);
        assert(catalog.select(first.id).id == first.id);
    }
    {
        monkeybyte::SessionCatalog restored(root / "sessions.json");
        assert(restored.list()["sessions"].size() == 2);
        assert(restored.list()["sessions"][1]["name"] == "Renamed");
        assert(restored.list()["sessions"][1]["model"] == "test-model");
        assert(restored.active()->name == "New Chat");
        restored.remove(restored.active()->id);
        assert(restored.active()->name == "Renamed");
    }
    // Electron startup must expose only Daedalus's restored/sanitized chain.
    // Seed a session with an unanswered tool call, a second text-carrying
    // unanswered call, and an orphaned result. No provider traffic is needed:
    // Agent::initialize performs the same restore used by the GTK frontend.
    const auto home = root / "daedalus-home";
    ::setenv("DAEDALUS_HOME", home.c_str(), 1);
    auto store = daedalus::SettingsStore::for_home(home);
    daedalus::ProviderConfig provider;
    provider.name = "fixture-provider";
    provider.host = "http://127.0.0.1:1";
    provider.api_key = "fixture-key";
    provider.chat_endpoint = "/v1/chat/completions";
    store.upsert_provider(provider);
    daedalus::ModelConfig model;
    model.name = "fixture-model";
    model.provider = provider.name;
    model.model_id = "fixture-model-id";
    model.max_context_size = 100000;
    store.upsert_model(model);
    store.set_default_model_name(model.name);

    monkeybyte::SessionCatalog catalog(root / "sessions.json");
    const auto restored_record = catalog.ensure(root / "workspace");
    const nlohmann::json dangling_call = {
        {"id", "dangling-call"}, {"type", "function"},
        {"function", {{"name", "execute_bash"}, {"arguments", "{\"command\":\"true\"}"}}}
    };
    const nlohmann::json tell_call = {
        {"id", "tell-call"}, {"type", "function"},
        {"function", {{"name", "tell_user"}, {"arguments", "{\"content\":\"restored user-facing answer\"}"}}}
    };
    const nlohmann::json damaged = nlohmann::json::array({
        {{"role", "system"}, {"content", "fixture instructions"}},
        {{"role", "user"}, {"content", "keep this request"}},
        {{"role", "assistant"}, {"content", ""}, {"tool_calls", nlohmann::json::array({dangling_call})}},
        {{"role", "tool"}, {"tool_call_id", "dangling-call"}, {"content", "orphaned result"}},
        {{"role", "assistant"}, {"content", "keep this completed answer"},
         {"tool_calls", nlohmann::json::array({dangling_call})}},
        {{"role", "assistant"}, {"content", ""}, {"tool_calls", nlohmann::json::array({tell_call})}},
        {{"role", "tool"}, {"tool_call_id", "tell-call"},
         {"content", "{\"status\":\"success\",\"content\":\"restored user-facing answer\"}"}},
        {{"role", "user"}, {"content", "keep the next request"}},
    });
    store.set_messages_json(restored_record.id, damaged.dump());

    monkeybyte::AgentService service(
        root / "sessions.json",
        [](std::string, nlohmann::json) {},
        [](std::string, nlohmann::json) { return nlohmann::json::object(); });
    const auto ensured = service.ensure_session(root / "workspace", restored_record.id);
    assert(ensured.at("status").contains("stats"));

    // GTK and Electron must resolve the same local voice configuration. GTK's
    // canonical values override Electron's legacy JSON, and an Electron save
    // writes both without exposing the key through public settings.
    store.set_string("monkeybyte.chimpflow", R"json({"enabled":false,"apiKey":"json-key"})json");
    store.set_string("chimpflow.enabled", "1");
    store.set_string("chimpflow.api_key", "gtk-key");
    store.set_string("chimpflow.voice", "gtk-voice");
    const auto gtk_config = service.chimpflow_config();
    assert(gtk_config.at("enabled") == true);
    assert(gtk_config.at("apiKey") == "gtk-key");
    assert(gtk_config.at("voice") == "gtk-voice");
    const auto saved_voice = service.update_settings({{"chimpflow", {
        {"enabled", true}, {"readAssistant", true}, {"dictationEnabled", true},
        {"serverUrl", "http://127.0.0.1"}, {"port", 8090}, {"apiKey", "electron-key"},
        {"ttsModel", "kokoro-82m"}, {"sttModel", "whisper"}, {"voice", "alba"},
    }}});
    assert(!saved_voice.at("chimpflow").contains("apiKey"));
    assert(saved_voice.at("chimpflow").at("hasApiKey") == true);
    assert(store.get_string("chimpflow.api_key", "") == "electron-key");
    assert(store.get_string("chimpflow.voice", "") == "alba");
    assert(service.chimpflow_config().at("apiKey") == "electron-key");

    const auto& history = ensured.at("history");
    assert(history.size() == 4);
    assert(history[0].at("content") == "keep this request");
    assert(history[1].at("content") == "keep this completed answer");
    assert(history[2].at("content") == "restored user-facing answer");
    assert(history[3].at("content") == "keep the next request");
    for (const auto& entry : history) {
        assert(entry.at("role") != "system");
        assert(!entry.at("content").get<std::string>().empty());
    }

    // Creating and deleting another chat must never clone or clear this one.
    const auto temporary = service.create_session(root / "workspace", "Temporary");
    assert(temporary.at("history").empty());
    const auto temporary_id = temporary.at("session").at("id").get<std::string>();
    auto project_store = daedalus::SettingsStore::for_home(
        root / "workspace" / ".daedalus" / "daedalus-monkeybyte");
    project_store.set_messages_json(temporary_id, nlohmann::json::array({
        {{"role", "user"}, {"content", "temporary session only"}},
    }).dump());
    const auto back_at_original = service.select_session(restored_record.id);
    assert(back_at_original.at("history").size() == 4);
    const auto temporary_again = service.select_session(temporary_id);
    assert(temporary_again.at("history").size() == 1);
    assert(temporary_again.at("history")[0].at("content") == "temporary session only");
    const auto after_delete = service.delete_session(temporary_id);
    assert(after_delete.at("history").size() == 4);
    assert(after_delete.at("history")[2].at("content") == "restored user-facing answer");
    const auto original_again = service.select_session(restored_record.id);
    assert(original_again.at("history").size() == 4);

    std::filesystem::remove_all(root);
    std::cout << "session catalog and restored-history tests passed\n";
}
