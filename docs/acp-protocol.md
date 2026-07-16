# Agent Communication Protocol (ACP)

**Status**: ACP is now part of **A2A** under the Linux Foundation.

- **Website**: https://agentcommunicationprotocol.dev
- **GitHub**: https://github.com/i-am-bee/acp
- **Migration Guide**: https://github.com/i-am-bee/beeai-platform/blob/main/docs/community-and-support/acp-a2a-migration-guide.mdx

## Overview

ACP is an open protocol for **agent interoperability** - enabling AI agents built with different frameworks to communicate through a standardized REST API.

## API Specification

Full OpenAPI spec: [`acp-openapi.yaml`](acp-openapi.yaml)

### Endpoints

| Method | Path | Description |
|--------|------|-------------|
| `GET` | `/ping` | Health check |
| `GET` | `/agents` | List available agents |
| `GET` | `/agents/{name}` | Get agent manifest |
| `POST` | `/runs` | Create a new agent run |
| `GET` | `/runs/{run_id}` | Get run status |
| `POST` | `/runs/{run_id}` | Resume a paused run |
| `POST` | `/runs/{run_id}/cancel` | Cancel a run |
| `GET` | `/runs/{run_id}/events` | List run events (SSE) |
| `GET` | `/sessions/{session_id}` | Get session details |

### Run Modes
- **sync**: Synchronous, returns complete response
- **async**: Asynchronous, returns 202 Accepted
- **stream**: Server-Sent Events (SSE) streaming

### Run Statuses
`created` → `in-progress` → `awaiting` → `completed` | `cancelled` | `failed`

## Key Features

- **REST-based Communication**: Standard HTTP patterns, no specialized protocols
- **All Message Types**: Uses MimeTypes for content identification (text, images, audio, video, binary)
- **No SDK Required**: Works with curl, Postman, or browser requests
- **Python/TypeScript SDKs**: Available for programmatic integration
- **Offline Discovery**: Agents discoverable even when inactive via embedded metadata
- **Async-first**: Built for asynchronous communication with long-running tasks

## Core Concepts

### Message Structure
```json
{
  "role": "user|agent|agent/{name}",
  "parts": [
    {
      "content_type": "text/plain",
      "content": "Hello world"
    }
  ]
}
```

### Agent Manifest
Describes agent capabilities including:
- Input/output content types (MIME types)
- Programming language
- Framework (BeeAI, crewAI, Autogen, etc.)
- Capabilities and domains
- Dependencies and recommended models

## Related Protocols

| Protocol | Purpose | Focus |
|----------|---------|-------|
| **MCP** | Model Context Protocol | Single agent to tools/data |
| **ACP/A2A** | Agent Communication Protocol | Agent-to-agent communication |
| **A2A** | Agent-to-Agent Protocol | Same as ACP, rebranded under Linux Foundation |

## C++ Implementation

**No C++ SDK exists** - only Python and TypeScript official SDKs.

However, since ACP is REST/JSON-based, a C++ client could be implemented using:
- HTTP client (libcurl - already in icarus)
- JSON parsing (nlohmann_json - already in icarus)
- SSE/streaming support

## Resources

- Documentation: https://agentcommunicationprotocol.dev
- OpenAPI Spec: [`acp-openapi.yaml`](acp-openapi.yaml)
- Python SDK: https://github.com/i-am-bee/acp/tree/main/python
- TypeScript SDK: https://github.com/i-am-bee/acp/tree/main/typescript
