#include "monkeybyte/ui/UiControl.h"

#include <condition_variable>
#include <glibmm.h>
#include <thread>

namespace monkeybyte {

namespace {
// Set by mark_main_thread(); execute() uses it to avoid deadlocking when a
// command is invoked from the main thread itself (e.g. D-Bus actions).
std::thread::id g_main_thread;

bool on_main_thread() { return std::this_thread::get_id() == g_main_thread; }
} // namespace

void UiControl::mark_main_thread() { g_main_thread = std::this_thread::get_id(); }

UiControl& UiControl::instance() {
    static UiControl inst;
    return inst;
}

void UiControl::register_command(std::string name, std::string description,
                                 Handler handler) {
    std::lock_guard lock(mutex_);
    commands_[std::move(name)] = Command{std::move(description), std::move(handler)};
}

std::vector<std::string> UiControl::list_commands() const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> out;
    for (const auto& [name, _] : commands_) out.push_back(name);
    return out;
}

std::string UiControl::describe(const std::string& name) const {
    std::lock_guard lock(mutex_);
    const auto it = commands_.find(name);
    return it != commands_.end() ? it->second.description : "";
}

nlohmann::json UiControl::execute(const std::string& name,
                                  const nlohmann::json& args) {
    Handler handler;
    {
        std::lock_guard lock(mutex_);
        const auto it = commands_.find(name);
        if (it == commands_.end())
            return {{"error", "unknown command: " + name}};
        handler = it->second.handler;
    }

    // Already on the main thread (e.g. D-Bus action): run directly —
    // posting to idle and blocking here would deadlock the main loop.
    if (on_main_thread()) {
        try {
            return handler(args);
        } catch (const std::exception& e) {
            return {{"error", e.what()}};
        }
    }

    // Marshal onto the GTK main loop and wait for the result.
    struct Gate {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        nlohmann::json result;
    };
    auto gate = std::make_shared<Gate>();
    Glib::signal_idle().connect_once([gate, handler = std::move(handler), args] {
        nlohmann::json result;
        try {
            result = handler(args);
        } catch (const std::exception& e) {
            result = {{"error", e.what()}};
        }
        {
            std::lock_guard lock(gate->mutex);
            gate->result = std::move(result);
            gate->done = true;
        }
        gate->cv.notify_all();
    });
    std::unique_lock lock(gate->mutex);
    gate->cv.wait(lock, [&] { return gate->done; });
    return gate->result;
}

} // namespace monkeybyte
