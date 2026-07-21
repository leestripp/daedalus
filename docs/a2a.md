# A2A (Agent2Agent Protocol) Support

This document describes how daedalus implements the [Agent2Agent (A2A) Protocol](https://a2a-protocol.org/latest/), an open standard for agent-to-agent communication originally developed by Google and now maintained by the Linux Foundation.

## What is A2A?

A2A lets autonomous agents built on different frameworks, languages, or vendors discover each other, delegate tasks, and exchange results without sharing internal memory or tools. It is complementary to MCP:

- **MCP** standardises agent-to-tool communication.
- **A2A** standardises agent-to-agent communication.

A typical flow is:

1. A client agent fetches the remote agent's **Agent Card** from `/.well-known/agent-card.json`.
2. The client sends a **Message** to the remote agent.
3. The remote agent creates a **Task** and returns it (synchronously or via streaming).
4. The client polls, streams, or receives push notifications until the task reaches a terminal state.

## Transport bindings

The A2A specification defines three transport bindings. All run over HTTP(S):

| Binding | Canonical endpoint style | Status in daedalus |
|---------|--------------------------|--------------------|
| **JSON-RPC 2.0** | `POST /` or `POST /a2a` with `{jsonrpc, method, params}` | Not yet implemented |
| **gRPC** | Generated from normative `a2a.proto` | Not yet implemented |
| **HTTP+JSON/REST** | `POST /v1/message:send`, `GET /v1/tasks/{id}`, ... | **Implemented** |

daedalus currently supports the **HTTP+JSON/REST** binding. JSON-RPC 2.0 and gRPC are planned future work.

## Agent discovery

Every A2A server must publish an Agent Card at:

```text
GET /.well-known/agent-card.json
```

The card is a JSON object describing the agent's identity, endpoint URL, preferred transport, capabilities, skills, and security requirements. See the [A2A Agent Card specification](https://a2a-protocol.org/latest/specification/#agent-discovery-the-agent-card) for the full schema.

Example:

```json
{
  "protocolVersion": "1.0",
  "name": "daedalus-coder",
  "description": "A daedalus-powered coding agent",
  "url": "https://coder.example.com/v1",
  "preferredTransport": "HTTP+JSON",
  "additionalInterfaces": [
    {"url": "https://coder.example.com/v1", "transport": "HTTP+JSON"}
  ],
  "version": "1.0.0",
  "capabilities": {
    "streaming": true,
    "pushNotifications": false,
    "stateTransitionHistory": false
  },
  "defaultInputModes": ["text/plain"],
  "defaultOutputModes": ["text/plain"],
  "skills": [
    {
      "id": "code-review",
      "name": "Code Review",
      "description": "Reviews code and suggests improvements",
      "tags": ["code", "review"],
      "examples": ["Review this function for bugs"],
      "inputModes": ["text/plain"],
      "outputModes": ["text/plain"]
    }
  ]
}
```

Use `daedalus::a2a::A2aClient::get_agent_card()` to fetch this card.

## HTTP+JSON/REST endpoint mapping

daedalus implements the following REST endpoints. All request/response bodies are JSON and use `Content-Type: application/json` (or `application/a2a+json` for A2A payloads).

| Operation | Method | Endpoint |
|-----------|--------|----------|
| Get Agent Card | `GET` | `/.well-known/agent-card.json` |
| Send Message | `POST` | `/v1/message:send` |
| Send Streaming Message | `POST` | `/v1/message:stream` |
| Get Task | `GET` | `/v1/tasks/{id}` |
| List Tasks | `GET` | `/v1/tasks` |
| Cancel Task | `POST` | `/v1/tasks/{id}:cancel` |
| Subscribe to Task | `GET` or `POST` | `/v1/tasks/{id}:subscribe` |

> **Note:** Earlier daedalus builds used endpoints without the `/v1/` prefix. The client and server now use the spec-compliant `/v1/` prefix. The server accepts both `GET` and `POST` for subscribe; the client sends `GET`.

## Canonical data model

The A2A data model is defined by the normative Protocol Buffer specification. The JSON representations below are what daedalus emits and expects over the REST binding.

### Part

A `Part` is a discriminated union. The `kind` field identifies the variant.

```json
{"kind": "text", "text": "Hello", "metadata": {}}
{"kind": "file", "file": {"name": "x.png", "mimeType": "image/png", "bytes": "...base64..."}}
{"kind": "data", "data": {"foo": 1}}
```

### Message

```json
{
  "kind": "message",
  "messageId": "msg-1",
  "role": "user",
  "parts": [{"kind": "text", "text": "Hello"}],
  "metadata": {},
  "extensions": []
}
```

Valid roles: `user`, `agent`.

### Task

```json
{
  "kind": "task",
  "id": "task-123",
  "contextId": "ctx-456",
  "status": {"state": "working", "timestamp": "2025-01-01T00:00:00Z"},
  "artifacts": [],
  "history": []
}
```

### TaskState

JSON values are lowercase/kebab-case:

- `submitted`
- `working`
- `input-required`
- `completed`
- `canceled`
- `failed`
- `rejected`
- `auth-required`
- `unknown`

### StreamResponse

Streaming endpoints return Server-Sent Events. Each `data:` line contains exactly one `StreamResponse` object with one of the following fields:

```json
{"task": {...}}
{"message": {...}}
{"statusUpdate": {"taskId": "...", "contextId": "...", "status": {...}}}
{"artifactUpdate": {"taskId": "...", "contextId": "...", "artifact": {...}}}
```

## Using the daedalus A2A client

```cpp
#include <daedalus.h>
#include <daedalus/a2a/A2aClient.h>

namespace a2a = daedalus::a2a;

int main() {
    a2a::A2aClient client("https://coder.example.com");

    // Discover the agent
    auto card = client.get_agent_card();
    if (!card) { /* handle error */ }

    // Send a message
    a2a::SendMessageRequest req;
    req.message.message_id = "msg-1";
    req.message.role = a2a::Role::user;
    req.message.parts.push_back(a2a::Part::from_text("Review this function"));

    auto result = client.send_message(req);
    if (!result) { /* handle error */ }

    // Or stream the response
    struct MyHandler : a2a::IA2aEventHandler {
        void on_task(const a2a::Task& t) override {}
        void on_status_update(const a2a::TaskStatusUpdateEvent& e) override {}
        void on_artifact_update(const a2a::TaskArtifactUpdateEvent& e) override {}
        void on_complete() override {}
    } handler;

    auto ok = client.send_streaming_message(req, handler);
}
```

## Using the daedalus A2A server

The server is exposed as `daedalus::a2a::A2aServer`. Hosts implement the `IA2aTaskBackend` interface to handle each A2A endpoint:

```cpp
#include <daedalus.h>
#include <daedalus/a2a/A2aServer.h>

struct MyBackend : daedalus::a2a::IA2aTaskBackend {
    daedalus::a2a::AgentCard get_agent_card() const override { ... }

    daedalus::a2a::A2aResult<daedalus::a2a::SendMessageResult>
    send_message(const daedalus::a2a::SendMessageRequest& req) override { ... }

    daedalus::a2a::A2aResult<daedalus::a2a::Task>
    get_task(const daedalus::a2a::GetTaskRequest& req) override { ... }

    daedalus::a2a::A2aResult<daedalus::a2a::ListTasksResponse>
    list_tasks(const daedalus::a2a::ListTasksRequest& req) override { ... }

    daedalus::a2a::A2aResult<daedalus::a2a::Task>
    cancel_task(const daedalus::a2a::CancelTaskRequest& req) override { ... }

    daedalus::a2a::A2aResult<void>
    send_streaming_message(const daedalus::a2a::SendMessageRequest& req,
                           daedalus::a2a::IA2aEventHandler& h) override { ... }

    daedalus::a2a::A2aResult<void>
    subscribe_to_task(const daedalus::a2a::SubscribeToTaskRequest& req,
                      daedalus::a2a::IA2aEventHandler& h) override { ... }
};

int main() {
    daedalus::a2a::A2aServerConfig cfg;
    cfg.address = "127.0.0.1";
    cfg.port = 8080;
    cfg.backend = std::make_shared<MyBackend>();

    daedalus::a2a::A2aServer server(std::move(cfg));
    server.start(); // blocks until stop()
}
```

The server handles HTTP routing, JSON serialization, and SSE framing. Streaming backends call the supplied `IA2aEventHandler` with `task`, `message`, `statusUpdate`, and `artifactUpdate` events; the server wraps them in SSE `data:` frames.

The CLI demo can also run as an A2A server. It creates a fresh `daedalus::Agent` for each task using the same configuration and tool set as the interactive REPL:

```bash
./build/demos/daedalus-cli/daedalus-cli --a2a-server --a2a-port 8080
```

Then talk to it from another shell:

```bash
./build/demos/daedalus-cli/daedalus-cli --a2a-url http://127.0.0.1:8080
/a2a-card
/a2a-send "hello"
/a2a-stream "hello"
```

## SDK smoke test

A Python SDK-based smoke test lives under `scripts/` and exercises the server against the official Google A2A Python SDK:

- `scripts/run_a2a_smoke_test.sh` — starts a temporary `daedalus-cli --a2a-server`, discovers its ephemeral port, and runs `scripts/test_a2a_sdk.py`.
- `scripts/test_a2a_sdk.py` — creates an SDK client, calls `send_message`, `send_streaming_message`, and `subscribe_to_task`.

Set up the venv once:

```bash
uv venv scripts/.venv
uv pip install --python scripts/.venv/bin/python a2a-sdk
```

Then run it through CMake:

```bash
cmake --build build --target a2a_smoke_test
```

The target builds `daedalus-cli`, starts the server on a random free port, and asserts that the SDK can resolve the card, send messages, stream events, and subscribe to a task.

> **Note:** The tester builds the AgentCard in memory rather than resolving `/.well-known/agent-card.json`, because daedalus currently emits the field names from an earlier A2A draft (`preferredTransport`/`additionalInterfaces`) while the current SDK expects `supported_interfaces`/`protocol_binding`.

## Service parameters

The client sends the `A2A-Version` header on every request. Servers interpret an empty/missing value as version `0.3` and a present value (e.g. `1.0`) as the requested protocol version, returning `VersionNotSupportedError` if they cannot honour it.

Optional extension opt-in is sent via the `A2A-Extensions` header.

## Compliance notes

daedalus's current A2A support is focused on the **HTTP+JSON/REST** binding. Known limitations:

- JSON-RPC 2.0 transport is not implemented.
- gRPC transport is not implemented.
- Push notification configuration endpoints are not implemented.
- Authenticated extended agent cards are not implemented.

These are documented as future work and do not affect interoperability with other HTTP+JSON/REST A2A agents.

## References

- [A2A Protocol — What is A2A?](https://a2a-protocol.org/latest/topics/what-is-a2a/)
- [A2A Protocol Specification v1.0.0](https://a2a-protocol.org/latest/specification/)
- [A2A Protocol Specification v0.3.0](https://a2a-protocol.org/v0.3.0/specification/)
- [A2A GitHub repository](https://github.com/a2aproject/A2A)
