# Agent2Agent Protocol (A2A) in icarus

**Status**: icarus has migrated from the legacy ACP v1 implementation to the
**A2A (Agent2Agent Protocol)** client and server built into the daedalus
library. ACP v1 (JSON-RPC over stdio, REST `/agents`/`/runs`) has been
removed.

- **A2A Specification**: https://a2a-protocol.org/latest/specification/
- **GitHub**: https://github.com/a2a-protocol/A2A

## Overview

A2A is an open protocol for **agent interoperability** — enabling AI agents
built with different frameworks to communicate through a standardized
HTTP+JSON/REST API.

## API Specification

The canonical A2A endpoints include:

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/.well-known/agent-card.json` | Agent discovery |
| `POST` | `/message:send` | Send a message (non-streaming) |
| `POST` | `/message:stream` | Send a message (streaming) |
| `GET` | `/tasks` | List tasks |
| `GET` | `/tasks/{id}` | Get task status |
| `POST` | `/tasks/{id}:cancel` | Cancel a task |
| `POST` | `/tasks/{id}:subscribe` | Subscribe to task updates |

The legacy ACP v1 OpenAPI spec in [`acp-openapi.yaml`](acp-openapi.yaml) is
retained for reference but is no longer implemented by icarus.

## Key Features

- **REST-based Communication**: Standard HTTP patterns, no specialized protocols
- **All Message Types**: Uses Part kinds for content identification (text, file, data)
- **No SDK Required**: Works with curl, Postman, or browser requests
- **Python/TypeScript SDKs**: Available for programmatic integration
- **Offline Discovery**: Agents discoverable via Agent Cards
- **Async-first**: Built for asynchronous communication with long-running tasks

## Related Protocols

| Protocol | Purpose | Focus |
|----------|---------|-------|
| **MCP** | Model Context Protocol | Single agent to tools/data |
| **A2A** | Agent-to-Agent Protocol | Agent-to-agent communication |

## C++ Implementation

icarus consumes the A2A implementation from daedalus
(`daedalus::a2a::A2aClient` and `daedalus::a2a::A2aServer`). The old
standalone `src/acp/` library has been removed.

### A2A Client Tool (`src/a2a_tools.cpp`)

The A2A client is exposed to the daedalus agent as the **`a2a` tool**,
which provides 7 actions:

| Action | Description |
|---|---|
| `get_agent_card` | Fetch the remote agent card |
| `send_message` | Send a message and start a task |
| `send_streaming_message` | Send a message and collect streaming events |
| `get_task` | Poll task status |
| `list_tasks` | List tasks |
| `cancel_task` | Cancel a task |
| `subscribe_to_task` | Subscribe to task updates |

Usage from the agent:
```json
{"action":"get_agent_card"}
{"action":"send_message","message":{"role":"user","parts":[{"text":"Hello"}]}}
```

### A2A Agent Daemon (`src/a2a_agent.cpp`)

When launched as `icarus --a2a-agent`, icarus runs as an A2A agent HTTP
server. It implements `daedalus::a2a::IA2aTaskBackend` and drives a
`daedalus::Agent` for each incoming task.

```bash
./build/icarus --a2a-agent
./build/icarus --a2a-agent --a2a-port 8080
# Port 0 (default) lets the OS assign an ephemeral port; the bound URL is printed to stderr.
```

The per-task agents honour `--model`, `--home`, `--skills`,
`--system-prompt`, `--no-memory`, and `--agent-name` (same semantics as
interactive mode), and the agent card served at
`/.well-known/agent-card.json` advertises the real bound URL.

The legacy `--acp-agent` flag is still accepted as a deprecated alias, but
the protocol is now A2A over HTTP rather than ACP v1 over stdio JSON-RPC.

### Building

CMake automatically fetches the daedalus dependency (which includes OmniNet
and the A2A implementation).

```bash
cmake -S . -B build && cmake --build build -j$(nproc)
./build/icarus --model-list                    # normal mode
./build/icarus --a2a-agent                     # A2A agent daemon mode
```

## Resources

- A2A Specification: https://a2a-protocol.org/latest/specification/
- A2A project (spec, samples, SDK links): https://github.com/a2a-protocol/A2A
