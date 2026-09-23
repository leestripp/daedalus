#include "monkeybyte/ui/chat/AppUiTool.h"

#include "monkeybyte/ui/UiControl.h"

namespace monkeybyte {

nlohmann::json AppUiTool::execute(const nlohmann::json& args,
                                  daedalus::HostToolContext&) {
    const std::string command = args.value("command", "");
    if (command.empty()) return {{"status", "error"}, {"error", "missing command"}};
    const nlohmann::json cmd_args = args.contains("args") ? args["args"]
                                                          : nlohmann::json::object();
    auto result = UiControl::instance().execute(command, cmd_args);
    result["status"] = result.contains("error") ? "error" : "ok";
    return result;
}

} // namespace monkeybyte
