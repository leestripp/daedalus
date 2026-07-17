# Token Counts in SSE Streaming

Research into whether missing token counts during SSE streaming is a MiniMax-specific issue or a broader API concern affecting OpenAI and Anthropic.

---

## Summary

**Token counts during SSE streaming are NOT universally available** — they depend on the provider and specific API settings. This is not purely a MiniMax bug; it is an industry-wide issue with varying solutions.

| Provider | Token Counts in Streaming | Requirements |
|----------|---------------------------|--------------|
| **OpenAI** | ✅ Yes (when configured) | Must send `stream_options: { include_usage: true }` |
| **Anthropic Claude** | ⚠️ Partial | `message_delta` has cumulative counts, but streaming API token tracking is inconsistent |
| **MiniMax** | ✅ Yes (when configured) | Must send `stream_options: { include_usage: true }` + recent API fix for slow chunk delivery |

---

## OpenAI

OpenAI added `stream_options: { include_usage: true }` in **May 2024** to address this gap.

### How it works

When `stream_options: { include_usage: true }` is included in the request, OpenAI sends a final SSE event after the stream ends:

```json
data: {"id":"chatcmpl-xxx","object":"chat.completion.chunk","created":1234567890,"model":"gpt-4o","choices":[],"usage":{"prompt_tokens":50,"completion_tokens":120,"total_tokens":170}}
```

Without this option, token counts are **not** included in streaming responses.

### Additional option

OpenAI also supports `continuous_usage_stats: true` which emits usage on **every** chunk. However, this is not universally supported by all providers and serves as a forward-compatibility feature.

### References

- [OpenAI Community: Usage stats now available when using streaming (May 2024)](https://community.openai.com/t/usage-stats-now-available-when-using-streaming-with-the-chat-completions-api-or-completions-api/738156)
- [OpenAI Community: Issue with Token Usage in Streaming Responses](https://community.openai.com/t/issue-with-token-usage-in-streaming-responses/892449)

---

## Anthropic Claude

Anthropic's streaming API behaves differently. According to their docs, the `message_delta` event includes token counts, but they are **cumulative** and may not reflect the final total until the stream completes.

### Known Issues

- There are reports that token counts may be inconsistent or off during streaming
- The `stream-json` format may not include usage data in the same way as OpenAI
- A GitHub issue noted that Claude Code's JSONL logs undercount tokens by up to 174x for input tokens

### References

- [Claude Streaming Messages Documentation](https://platform.claude.com/docs/en/build-with-claude/streaming)
- [GitHub: Anthropic API provider not counting input/cache tokens](https://github.com/cline/cline/issues/4346)
- [Gille.ai: Claude Code's Token Counts Are Wrong](https://gille.ai/en/blog/claude-code-jsonl-logs-undercount-tokens/)

---

## MiniMax

MiniMax follows the OpenAI-compatible API specification and supports `stream_options: { include_usage: true }`.

### Known Issues (Daedalus Project)

The Daedalus project encountered issues with token counts not being received during MiniMax streaming. Root causes identified:

1. **EOF handling bug**: `HttpSession::submit_cancellable` was treating socket-wait timeouts as EOF, causing early termination of the streaming loop before the usage trailer arrived.

2. **Slow chunk delivery**: MiniMax/M2.7 sends chunks with ~150-300ms spacing, which could trigger timeout issues on slow connections.

3. **Fix implemented**: Distinguishing `nullopt`-timeout from real EOF in the streaming loop resolves the issue.

### Current Implementation Status

The Daedalus codebase has:
- Fixed EOF handling in `HttpSession::submit_cancellable`
- Ensured `stream_options: { include_usage: true }` is sent
- Added `continuous_usage_stats: true` for forward compatibility
- Implemented trailer recovery via `MiniMaxSseParser`

### References

- [MiniMax OpenAI-compatible API Docs](https://platform.minimax.io/docs/api-reference/text-openai-api)
- [MiniMax-M2 Issue #105: reasoning_split](https://github.com/MiniMax-AI/MiniMax-M2/issues/105)

---

## Azure OpenAI

Azure OpenAI also supports `stream_options: { include_usage: true }`, but the feature had to be explicitly added (around July 2024). Without this parameter, streaming responses do not include token usage.

### References

- [Azure OpenAI: Streaming ChatCompletions and Token Consumption Tracking](https://journeyofthegeek.com/2024/09/25/azure-openai-service-streaming-chatcompletions-and-token-consumption-tracking/)
- [Azure OpenAI Community: Streaming token usage](https://learn.microsoft.com/en-us/answers/questions/1805363/azure-openai-streaming-token-usage)

---

## vLLM and Self-Hosted Models

For self-hosted models using vLLM or similar backends:

- Token counts during streaming depend on the backend implementation
- OpenAI-compatible servers should respect `stream_options: { include_usage: true }`
- Some backends may silently ignore this option

---

## Conclusion

### Is this only a MiniMax issue?

**No.** Token count unavailability during SSE streaming is a **broad industry issue** that affected (and in some cases still affects):

1. **OpenAI** — Addressed in May 2024 with `stream_options: { include_usage: true }`
2. **Azure OpenAI** — Addressed in mid-2024 with same parameter
3. **Anthropic Claude** — Token counts available but potentially inconsistent
4. **MiniMax** — Follows OpenAI spec; issue was Daedalus-specific (EOF handling bug)
5. **Self-hosted (vLLM)** — Varies by implementation

### Current Status (Daedalus Project)

The Daedalus codebase has been fixed to properly handle SSE streaming token counts for all OpenAI-compatible providers by:

1. Sending `stream_options: { include_usage: true }` in all streaming requests
2. Fixing EOF handling to distinguish timeouts from actual stream end
3. Implementing proper trailer chunk recovery

### Recommendations for Further Testing

1. **Test with OpenAI** — Verify token counts work with `stream_options: { include_usage: true }`
2. **Test with Anthropic** — Compare streaming vs non-streaming token counts
3. **Monitor MiniMax** — The fix should resolve MiniMax issues, but continued monitoring is advised
4. **Check provider docs** — Each provider may have different requirements or limitations

---

## Graceful Degradation

When token counts are unavailable during SSE streaming, the agent loop must **continue normally** with a warning. This is by design — missing token counts should never terminate the agent cycle.

### Implementation

The current implementation in `LLMClient::update_token_usage()` handles this correctly:

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

### Verification Checklist

- [x] `update_token_usage()` returns early with warning on null/missing usage
- [x] Agent loop continues after token count failure
- [x] Warning is suppressed for duplicate warnings per turn (`warned_no_usage_this_turn_`)
- [x] Usage parsing is wrapped in try/catch for resilience
- [x] Unit tests added: `daedalus_token_usage_graceful_tests` (15/15 pass, 38/38 assertions)

### Test Coverage: `tests/test_token_usage_graceful.cpp`

The test suite covers these graceful-degradation scenarios:

1. `usage` field missing → tokens=0, warn
2. `usage` is null → tokens=0, warn
3. `usage` is a string → tokens=0, warn
4. `usage` is a number → tokens=0, warn
5. `usage` is empty object → tokens=0, no warn
6. `usage` is partial → missing fields default to 0, no warn
7. `usage` is full → all fields parsed, no warn
8. response is a string → tokens=0, warn
9. response is null → tokens=0, warn
10. response is empty object → tokens=0, warn
11. `should_compress(0)` returns false (no false auto-compression)
12. Auto-compression threshold sanity checks
13. Warning is emitted exactly once per turn (B32)
14. Warning resets on new turn

### Task: Confirm Graceful Degradation (2026-07-17)

**Status**: ✅ COMPLETE — Added `daedalus_token_usage_graceful_tests` (15/15 pass, 38/38 assertions). Verified the contract via unit tests: when `usage` is null/missing/malformed, the agent loop continues without error. A warning is emitted (suppressed per-turn via `warned_no_usage_this_turn_`).

---

## Related Files

- `docs/sse_streaming.md` — SSE consumption pipeline details
- `src/api/HttpSession.cpp` — Streaming loop implementation (EOF fix location)
- `src/api/LLMClient.cpp` — Request payload construction
- `src/api/MiniMaxSseParser.{h,cpp}` — SSE parsing and trailer recovery

---

*Last updated: 2026-07-17*
