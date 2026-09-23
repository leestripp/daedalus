#!/usr/bin/env python3
"""
Smoke-test a daedalus A2A server using the official Google A2A Python SDK.

Usage:
    uv run scripts/test_a2a_sdk.py http://127.0.0.1:8000

Requires a running daedalus A2A server, e.g.:
    daedalus-cli --a2a-server --a2a-port 8000 <project_dir>

The script exercises:
  1. Agent card resolution
  2. send_message (non-streaming)
  3. send_streaming_message (SSE stream of events)
  4. subscribe_to_task
"""

from __future__ import annotations

import argparse
import asyncio
import sys

import a2a.types.a2a_pb2 as a2a_pb2
import httpx
from a2a.client.card_resolver import A2ACardResolver
from a2a.client.client import ClientConfig
from a2a.client.client_factory import create_client


def make_text_message(text: str) -> a2a_pb2.Message:
    msg = a2a_pb2.Message()
    msg.role = a2a_pb2.Role.ROLE_USER
    msg.parts.append(text_part(text))
    return msg


def text_part(text: str) -> a2a_pb2.Part:
    part = a2a_pb2.Part()
    part.text = text
    return part


async def build_client(base_url: str):
    """Resolve the server's AgentCard and create an A2A SDK client."""
    timeout = httpx.Timeout(300.0, connect=10.0)
    http_client = httpx.AsyncClient(timeout=timeout)
    resolver = A2ACardResolver(http_client, base_url)
    card = await resolver.get_agent_card()
    config = ClientConfig(
        streaming=True,
        supported_protocol_bindings=["HTTP+JSON"],
        httpx_client=http_client,
    )
    client = await create_client(card, client_config=config)
    return client, card


async def test_send_message(client: Client) -> str | None:
    print("\n--- send_message (non-streaming) ---")
    req = a2a_pb2.SendMessageRequest()
    req.message.CopyFrom(make_text_message("Hello from the A2A Python SDK"))
    config = a2a_pb2.SendMessageConfiguration()
    config.accepted_output_modes.append("text/plain")
    req.configuration.CopyFrom(config)

    # With streaming=True the SDK still returns an AsyncIterator; drain it
    # and report the final task event if present.
    final_task = None
    async for event in client.send_message(req):
        kind = event.WhichOneof("payload")
        if kind == "task":
            final_task = event.task
        elif kind == "error":
            print(f"  error: {event.error.message}")

    if final_task and final_task.id:
        print(f"task id: {final_task.id}")
        print(f"task state: {a2a_pb2.TaskState.Name(final_task.status.state)}")
        if final_task.artifacts:
            for art in final_task.artifacts:
                for part in art.parts:
                    if part.text:
                        print(f"artifact text: {part.text!r}")
        return final_task.id
    print("no final task event received")
    return None


async def test_streaming_message(client: Client) -> None:
    print("\n--- send_streaming_message ---")
    req = a2a_pb2.SendMessageRequest()
    req.message.CopyFrom(make_text_message("Stream me"))
    config = a2a_pb2.SendMessageConfiguration()
    config.accepted_output_modes.append("text/plain")
    req.configuration.CopyFrom(config)

    event_counts: dict[str, int] = {}
    async for event in client.send_message(req):
        kind = event.WhichOneof("payload")
        event_counts[kind] = event_counts.get(kind, 0) + 1
        if kind == "status_update":
            print(f"  status: {a2a_pb2.TaskState.Name(event.status_update.status.state)}")
        elif kind == "artifact_update":
            print(f"  artifact: {event.artifact_update.artifact.name}")
        elif kind == "task":
            print(f"  final task: {event.task.id}")
        elif kind == "error":
            print(f"  error: {event.error.message}")
        else:
            print(f"  event: {kind}")
    print(f"event counts: {event_counts}")


async def test_subscribe(client: Client, task_id: str) -> None:
    print("\n--- subscribe_to_task ---")
    req = a2a_pb2.SubscribeToTaskRequest()
    req.id = task_id
    event_counts: dict[str, int] = {}
    async for event in client.subscribe(req):
        kind = event.WhichOneof("payload")
        event_counts[kind] = event_counts.get(kind, 0) + 1
        print(f"  event: {kind}")
    print(f"event counts: {event_counts}")


async def main() -> int:
    parser = argparse.ArgumentParser(description="Smoke-test a daedalus A2A server")
    parser.add_argument("url", help="Base URL of the A2A server (e.g. http://127.0.0.1:8000)")
    args = parser.parse_args()

    base_url = args.url.rstrip("/")
    print(f"Connecting to daedalus A2A server at {base_url}/v1 ...")

    try:
        client, card = await build_client(base_url)
    except Exception as e:
        print(f"Failed to create A2A client: {e}", file=sys.stderr)
        return 1

    print(f"Agent: {card.name}")
    print(f"Description: {card.description}")
    print(f"Version: {card.version}")
    print(f"Streaming: {card.capabilities.streaming}")

    task_id = await test_send_message(client)
    await test_streaming_message(client)
    if task_id:
        await test_subscribe(client, task_id)
    else:
        print("\n--- subscribe_to_task skipped (no task id) ---")

    await client.close()
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
