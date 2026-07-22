# SSE Streaming (LLM)

How `LLMClient` consumes OpenAI-compatible Server-Sent Events from
chat-completions endpoints, and the gotchas future maintainers need
to know about.

**Audience:** core contributors adding a new provider, debugging
token-count / streaming-related issues, or extending `MiniMaxSseParser`.

---

## Pipeline overview

```
HTTP POST /v1/chat/completions  (stream: true)
   ↓
OmniNet::HTTP::Client::send_streaming()
   ↓
HttpSession::submit_cancellable() worker thread
   ↓ (per raw chunk)
OmniNet::HTTP::ClientResponseStream::read_some()
   ↓ (decoded chunked-encoding body bytes)
LLMClient::run_agent_cycle streaming_callback
   ↓ (string_view of bytes)
MiniMaxSseParser::on_chunk()
   ↓ (buffers across chunk boundaries, splits on '\n')
MiniMaxSseParser::parse_line("data: {...}")
   ↓ (parses each chunk, accumulates text/tool_calls, captures final_event_)
sse_parser.finish()          ← flushes trailing partial line
   ↓
LLMClient reads:
  sse_parser.final_event()   ← last parsed SSE data: line
  sse_parser.had_text_deltas() / accumulated_tool_calls()
  → builds synthesized_message() if the final event had no choices
   ↓
LLMClient::update_token_usage(response_json) updates last_total_tokens_
```

A response contains, in order, typically:

1. **N reasoning chunks** with `delta.reasoning_content` and
   `delta.reasoning_details`. With `reasoning_split: true`, these are
   kept out of `content`.
2. **0..M content chunks** with `delta.content` (text) or
   `delta.tool_calls`.
3. **One terminal chunk** with `choices:[{...delta.content, finish_reason:"stop"}]`
   and `usage: null`.
4. **One final usage-only trailer** with `choices:[]` and
   `usage:{prompt_tokens, completion_tokens, total_tokens, ...}`.
5. **`data: [DONE]`** marker.

Steps 3 and 4 are different SSE events; step 5 is a literal sentinel
no parser cares about.

---

## Invariants and gotchas

### 1. The SSE-streaming-loop EOF gotcha (2026-07-17)

**`HttpSession::submit_cancellable`'s streaming loop must distinguish
"nullopt from a socket wait timeout" from "real EOF".**

`OmniNet::HTTP::ClientResponseStream::read_some()` returns
`std::nullopt` **twice**:

- when the socket reaches true EOF (`m_state->eof` flips to `true`);
- when `socket->waitForRead(100ms)` times out without any data
  (`m_state->eof` stays `false`).

Treating both as EOF breaks any streaming response whose chunks
arrive >100ms apart (e.g. MiniMax/M2.7, where chunked-encoding
frames are spaced ~150–300 ms). The loop exits after only the
first 2 reads, the SSE parser flushes whatever it has, and the
final usage-only trailer (step 4 above) never arrives → token
counts zero.

The fix in `src/api/HttpSession.cpp`:

```cpp
while (true) {
    if (cancelled()) break;
    auto chunk = stream.read_some();
    if (!chunk) {
        if (stream.eof()) break;        // real EOF
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;                        // transient socket-wait timeout
    }
    if (chunk->empty()) continue;
    std::string_view sv(...);
    stream_callback(sv);
}
```

**Do not change this back to `if (!chunk) break;`** — the old
behaviour drops the trailer chunk on slow streams.

### 2. `usage: null` on the terminal chunk is normal

The chunk with `finish_reason:"stop"` has `"usage": null`. Only the
trailer (with `choices:[]`) has the real usage fields. The BUG-001
gate in `MiniMaxSseParser.h::is_usage_only_chunk()` keys on what the
parser accumulated (text OR tool_calls) instead of on `usage`
non-null-ness — so the trailer is correctly recovered even when
`usage` in the terminal chunk is `null`.

```cpp
inline bool is_usage_only_chunk(
    bool stream_enabled,
    const nlohmann::json& response,
    const MiniMaxSseParser& parser) noexcept
{
    if (!stream_enabled) return false;
    if (response.contains("choices") && !response["choices"].empty())
        return false;            // not a no-choices chunk
    return parser.had_text_deltas()
        || !parser.accumulated_tool_calls().empty();
}
```

### 3. Always send `stream_options: { include_usage: true }` when streaming

Without it, providers that respect the OpenAI spec (including
MiniMax) skip the usage trailer entirely and `update_token_usage`
logs `'usage' is null in SSE stream — token counts unavailable` on
every turn.

The current request payload (see `LLMClient::run_agent_cycle`) also
sets `continuous_usage_stats: true`. On OpenAI / vLLM this emits
usage on **every** chunk; on MiniMax/M2.7 it is silently a no-op
(only the final trailer gets usage). Keep it for forward-compat.

### 4. `reasoning_split: true` prevents thinking-leak into `content`

MiniMax-M2 mixes model-thinking into the streamed `content` field as
a `(think)\n...` prefix. Set
`request_payload["reasoning_split"] = true` (already wired) so
thinking goes to `reasoning_content` / `reasoning_details` instead
and `content` stays clean for live display. Without it, every
agent UI double-shows the chain-of-thought as if it were the
final answer.

Reference: [MiniMax-M2 issue #105](https://github.com/MiniMax-AI/MiniMax-M2/issues/105).

### 5. The streaming callback may fire with arbitrary-sized bytes

The chunk size `read_some()` returns is OS- and provider-dependent.
On Linux with TLS the kernel buffer often delivers the entire
SSE response in 2–5 chunks (≈100–2700 bytes total). The buffer
must therefore:

- never assume one callback = one SSE line (use
  `MiniMaxSseParser::on_chunk` which buffers across callbacks),
- always call `MiniMaxSseParser::finish()` after the async wait,
  to flush whatever partial line remains in the buffer.

### 6. Cancellation races

The streaming callback is fired from a worker thread. Wrap each
invocation in a `stop_token` check so an in-flight chunk from the
kernel buffer doesn't fire `on_chunk` after `Agent::cancel()` has
been called. `HttpSession::submit_cancellable` already does this
via a `safe_callback` wrapper.

### 7. Authorization header — pass the value, not the full header

`OmniNet::HTTP::ClientRequest::header(name, value)` prepends the
header **name** itself — pass only the value, never
`"Authorization: Bearer XXX"`. The earlier code did:

```cpp
req.header("Authorization",
           std::string("Authorization: Bearer ") + config_.api_key);
```

producing the malformed header line `Authorization: Authorization: Bearer XXX`,
which MiniMax rejects with `authorized_error` (401). The fix is:

```cpp
req.header("Authorization", "Bearer " + config_.api_key);
```

This applies to **both** the streaming path (when building
`HttpRequest` inside `make_api_request_async`) **and** the sync
fallback path (`make_http_request_with_retry` building
`OmniNet::HTTP::ClientRequest`). Both sites have been fixed.
Affects every request — visible symptom is a 401 + `authorized_error`
on the first API call from a fresh agent session.

---

## Debugging recipe

1. Add `static int call_count = 0;` inside the streaming callback
   and `[DBG] call=N size=... sample=...` to the sink via
   `sink_->on_log(Warn, ...)` (Info-level logs go to the file,
   not the sink).
2. Watch `sse_parser.had_text_deltas()` after `finish()` — it
   should be `true` for any non-empty completion. If `false`,
   either no chunks arrived (see gotcha 1) or the buffer was
   flushed mid-line.
3. Watch `sse_parser.final_event()` — its `usage` field is `null`
   if MiniMax only got the terminal chunk (expected), or non-null
   if the trailer arrived. If it's non-null in your test but
   `update_token_usage` still complains about null, you're
   looking at the wrong chunk — the trailer SHOULD be the last
   `data:` line the parser sees; everything after it is
   `data: [DONE]` which the parser ignores.

---

## Token-count graceful degradation

When token counts are unavailable during SSE streaming, the agent loop
**continues normally** with a warning. Missing token counts must never
terminate the agent cycle.

`LLMClient::update_token_usage()` implements this contract:

```cpp
void LLMClient::update_token_usage(const json& response_json) {
    if (!response_json.contains("usage")) {
        if (sink_) sink_->on_log(daedalus::LogLevel::Warn,
            "[LLM] No 'usage' field in response (SSE stream? — token counts are 0)");
        return;  // ← Continue, don't fail
    }
    if (response_json["usage"].is_null()) {
        if (sink_) sink_->on_log(daedalus::LogLevel::Warn,
            "[LLM] 'usage' is null in SSE stream — token counts unavailable");
        return;  // ← Continue, don't fail
    }
    // ... parse usage ...
}
```

Key behaviours:

- Missing or null `usage` → tokens default to `0`, one warning per turn.
- Malformed `usage` types (string, number, etc.) → caught and warned.
- Partial `usage` objects → missing fields default to `0`, no warning.
- Warnings are deduplicated per turn via `warned_no_usage_this_turn_`.

Unit-test coverage lives in `tests/test_token_usage_graceful.cpp`.

---

## Reference

- `src/api/HttpSession.cpp` — the streaming loop (this is where
  the EOF fix lives).
- `src/api/LLMClient.cpp::run_agent_cycle` — request payload
  shape, `stream_options`, `reasoning_split`, trailer recovery.
- `src/api/MiniMaxSseParser.{h,cpp}` — line buffer,
  `accumulated_text_`, `accumulated_tool_calls_`,
  `had_text_deltas()`, `final_event_`, `is_usage_only_chunk()`.
- `src/api/LLMClient.cpp::update_token_usage` — where `usage` is
  extracted into `last_total_tokens_` etc.
- MiniMax OpenAI-compatible API docs:
  <https://platform.minimax.io/docs/api-reference/text-openai-api>
  (only documents `stream_options.include_usage`; not
  `continuous_usage_stats`).
