#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace monkeybyte {

// Unified UI control surface (design: test hooks + agent tools + HTTP).
// MainWindow registers commands; three front-ends invoke them:
//   1. D-Bus GActions (win.* via org.gtk.Actions) — for scripted testing
//   2. the embedded daedalus Agent (app_ui_control tool)
//   3. the OmniNet HTTP control server (POST /api/ui/control)
//
// Commands execute on the GTK main thread; callers from any other thread
// are marshalled through Glib::signal_idle and block until done.
class UiControl {
public:
    using Handler = std::function<nlohmann::json(const nlohmann::json& args)>;

    static UiControl& instance();

    // Called once from the GTK main thread (MainWindow ctor).
    static void mark_main_thread();

    // Called once from MainWindow (main thread).
    void register_command(std::string name, std::string description, Handler handler);

    std::vector<std::string> list_commands() const;
    std::string describe(const std::string& name) const;

    // Run a command. Blocks the calling thread until the main loop answers.
    // Returns {"ok": ...} or {"error": ...}.
    nlohmann::json execute(const std::string& name, const nlohmann::json& args);

private:
    struct Command {
        std::string description;
        Handler handler;
    };
    mutable std::mutex mutex_;
    std::map<std::string, Command> commands_;
};

} // namespace monkeybyte
