# Creating Custom Tools for daedalus

daedalus exposes a public `daedalus::BaseTool` class. Subclass it, fill in four
virtual functions, and register the tool with `Agent::register_tool()`. The
library handles the rest: JSON schema generation, LLM dispatch, result
routing through `AgentSink`, and progress callbacks.

> **Note:** `daedalus::HostTool` is the old name for the same class. It still
> compiles but is deprecated; new code should use `daedalus::BaseTool`.

---

## The `BaseTool` interface

```cpp
#include <daedalus.h>

class MyTool : public daedalus::BaseTool {
public:
    // Unique tool name. This is what the LLM emits in tool_calls.
    [[nodiscard]] std::string get_name() const override;

    // Description shown to the LLM in the system prompt.
    [[nodiscard]] std::string get_description() const override;

    // OpenAI-compatible function definition (JSON Schema).
    [[nodiscard]] nlohmann::json get_definition() const override;

    // Run the tool. Return a JSON object; include "status": "success"/"error".
    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override;
};
```

---

## Example: a `timestamp` tool

```cpp
#include <daedalus.h>
#include <chrono>
#include <string>

class TimestampTool : public daedalus::BaseTool {
public:
    std::string get_name() const override { return "timestamp"; }

    std::string get_description() const override {
        return "Return the current UTC timestamp in ISO-8601 format.";
    }

    nlohmann::json get_definition() const override {
        return {
            {"type", "function"},
            {"function", {
                {"name", "timestamp"},
                {"description", get_description()},
                {"parameters", {
                    {"type", "object"},
                    {"properties", nlohmann::json::object()},
                    {"required", nlohmann::json::array()}
                }}
            }}
        };
    }

    nlohmann::json execute(const nlohmann::json& /*args*/,
                           daedalus::HostToolContext& /*ctx*/) override {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        std::string iso = std::put_time(std::gmtime(&t), "%FT%TZ");
        return {
            {"status", "success"},
            {"timestamp", iso}
        };
    }
};
```

---

## Registering the tool

```cpp
int main() {
    daedalus::AgentConfig cfg;
    cfg.project_root = ".";
    cfg.daedalus_home = daedalus::Paths::global().user_data_root();

    auto sink = std::make_shared<MySink>();
    daedalus::Agent agent(std::move(cfg), sink);

    agent.register_tool(std::make_unique<TimestampTool>());
    agent.initialize();

    // The LLM can now call the "timestamp" tool.
    agent.submit_user_message("What time is it?");
    agent.run_one_turn();
}
```

Tools may be registered **before** `initialize()` (they are queued and
registered when the internal `ToolFactory` is ready) or **after**
`initialize()` (they become available on the next turn).

---

## `HostToolContext`: project-scoped I/O

Custom tools receive a `daedalus::HostToolContext&` instead of a raw
`FileIO&`. This keeps the public API small and stable:

```cpp
class HostToolContext {
public:
    virtual std::string read_file(const std::filesystem::path& p) = 0;
    virtual void write_file(const std::filesystem::path& p,
                            const std::string& content) = 0;
    virtual std::vector<std::string>
        list_directory(const std::filesystem::path& p) = 0;
    virtual const std::filesystem::path& project_root() const = 0;
};
```

All paths are relative to `project_root()`. The library enforces the
project boundary and protected-file rules automatically.

---

## Result format

Tools should return a JSON object. The convention used by built-in tools is:

```json
{
  "status": "success",
  "...": "..."
}
```

For errors:

```json
{
  "status": "error",
  "message": "human-readable explanation",
  "permanent_failure": true
}
```

`permanent_failure: true` tells the agent not to retry the same call.

---

## Best practices

1. **Keep definitions minimal.** The LLM sees every tool on every turn;
   large schemas consume context window.
2. **Validate arguments.** Return clear errors instead of throwing.
3. **Use `ctx.project_root()`** for path resolution; don't assume the
   process working directory.
4. **Return structured data.** The LLM parses the result, so JSON objects
   are more useful than plain prose.
5. **Avoid side effects in `get_definition()` / `get_description()`.**
   These are called frequently.

---

## Full example: file-size reporter

```cpp
class FileSizeTool : public daedalus::BaseTool {
public:
    std::string get_name() const override { return "file_size"; }

    std::string get_description() const override {
        return "Return the size in bytes of a file within the project.";
    }

    nlohmann::json get_definition() const override {
        return {
            {"type", "function"},
            {"function", {
                {"name", "file_size"},
                {"description", get_description()},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"path", {
                            {"type", "string"},
                            {"description", "Relative path inside the project."}
                        }}
                    }},
                    {"required", {"path"}}
                }}
            }}
        };
    }

    nlohmann::json execute(const nlohmann::json& args,
                           daedalus::HostToolContext& ctx) override {
        std::string rel = args.value("path", "");
        if (rel.empty()) {
            return {{"status", "error"}, {"message", "missing 'path'"}};
        }

        std::filesystem::path p = ctx.project_root() / rel;
        std::error_code ec;
        auto size = std::filesystem::file_size(p, ec);
        if (ec) {
            return {{"status", "error"}, {"message", ec.message()}};
        }
        return {{"status", "success"}, {"path", rel}, {"size", size}};
    }
};
```

---

## Where to put custom tools

For small projects, define the tool in the same file as your host. For
larger projects, put each tool in its own header under your host's source
tree. The built-in tools in `src/tools/` are not a template for custom
tools — they use internal APIs; custom tools should only use the public
`daedalus::BaseTool` + `daedalus::HostToolContext` interface.

See `demos/daedalus-subagent/main.cpp` for a real host-registered tool
that spawns child agents.
