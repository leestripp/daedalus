# How Context Compression Works

daedalus has a context compression system that keeps conversations manageable during long tasks. The **"70/30 Split"** algorithm compresses the **first 70%** of messages while keeping the **last 30%** intact, preserving recent context for the agent to continue working.

---

## Why "70/30 Split"?

The old approach ("compress all") caused issues:
- Agent lost too much context
- Strange behavior after compression
- Tool call mismatches from orphaned results

The current "70/30 Split" approach:
- Compresses the **oldest 70%** of messages (the bulk of historical context)
- Keeps the **newest 30%** intact (so the agent can continue working seamlessly)
- Splits at a **safe point** (NOT in the middle of a tool call pair)

This gives the LLM a small, recent context window where it can pick up exactly where it left off.

---

## Algorithm Overview

```
BEFORE: [system, msg1, msg2, ..., msgN*0.7, msgN*0.7+1, ..., msgN]
              ───────────────────────────────────────────────
                    FIRST 70%                LAST 30%
                       ↓                       ↓
                CONVERT TO PLAIN TEXT      KEEP INTACT
                       ↓                       ↓
                  PASS TO LLM             (stays in messages)
                       ↓
                  GET SUMMARY
                       ↓
AFTER:  [system, user(summary), msgN*0.7+1, ..., msgN]
            ───────   ──────────────   ─────────────────
            SYSTEM    FIRST 70%        LAST 30% KEPT
                      SUMMARIZED
                      (replaced)
```

| Region | Action | Why |
|--------|--------|-----|
| **First 70%** | Converted to plain text → sent to LLM → replaced by summary | Most tokens are here; old context |
| **Last 30%** | Kept intact in messages array | Recent work, agent needs this to continue |
| **Split Point** | Adjusted to NOT break a tool call pair | Prevents orphaned tool_calls / orphan tool results |

### Why This Works

1. **No orphaned tool_calls**: The split is at a SAFE point (after a completed tool call chain or after any non-tool message)
2. **Summary captures the past**: The LLM summary condenses the bulk of old context
3. **Recent context preserved**: The last 30% gives the agent a tight window to continue work
4. **Clean message chain**: `[system, user(summary), suffix]` - the first message after system is a user message, which strict chat templates require

---

## Safe Split Point (Critical)

The 70/30 split point must NOT fall in the middle of a tool call chain. A tool call chain looks like:

```
[assistant, tool_calls: [{id: "X"}]]   ← tool call
[tool, tool_call_id: "X", content: ...] ← tool result
```

If we split between these two, the tool result becomes orphaned (its tool_call_id is in the "compressed" section that no longer exists in the messages array). This would cause the API to return 400 errors.

### Algorithm

```cpp
// 1. Calculate target split at 70%
size_t target_split = (total_non_system * 70) / 100;

// 2. Find all tool_call_ids that would be in the "first 70%" (compressed) range
std::unordered_set<std::string> first_70_tool_calls;
for (size_t i = 1; i <= target_split; ++i) {
    if (msg is assistant with tool_calls) {
        for each tc: first_70_tool_calls.insert(tc.id);
    }
}

// 3. Walk through messages looking for any tool result that references a first_70 tool_call
//    If found, the safe split point must be BEFORE that tool result
for (size_t i = 1; i <= target_split; ++i) {
    if (msg is tool && msg.tool_call_id in first_70_tool_calls) {
        // This tool result pairs with a tool call in the first 70%
        // Adjust safe_split_point to BEFORE this tool result
        safe_split_point = i - 1;
        break;
    }
}

// 4. Split at safe_split_point
```

### Edge Case: Insufficient Safe Split

If the first valid safe split point is too far from the 70% target (e.g., a huge tool call chain in the first half), we may end up splitting at a different percentage. The goal is to ALWAYS split at a safe point, even if it's not exactly 70%.

---

## Compression Flow

```
1. EXTRACT FIRST 70% (safe split)
   │
   ├── get_compression_indices() → (start_idx, end_idx)
   ├── Make COPY of messages [start_idx, end_idx)
   └── Check for safe split point (don't break tool call pairs)

2. DELETE IMMEDIATELY
   │
   └── messages_.erase(start_idx, end_idx)
       ⚠️ Context is already reduced - even if LLM fails!

3. CONVERT TO PLAIN TEXT
   │
   └── messages_to_plain_text(middle_copy)
       (Removes JSON overhead ~50%)

4. SEND TO LLM
   │
   ├── Build compression prompt
   └── call_llm_for_summary(prompt)

5. IF LLM SUCCEEDS → Add summary
   │
   └── Rebuild: [system, user(summary), ...suffix]

6. IF LLM FAILS → Continue anyway
   │
   └── Context already reduced. Return nullopt, caller continues main loop.

7. CONTINUE MAIN LOOP
   │
   └── DO NOT exit to user input. Continue agent cycle.
```

---

## Trigger Methods

### Method 1: Host-Triggered Compression (`Agent::compress_context()`)

**When to use:**
- Context is getting large but not yet at 90% threshold
- Host wants to clear history and start fresh with a summary
- After completing a major task milestone

**Semantics:**
- **Idle (no turn running):** compresses immediately, synchronously on
  the caller's thread; `on_compaction()` fires from that thread. If
  there is nothing to compress yet, the request carries over to the
  next turn's first cycle.
- **Busy (turn running):** deferred to the end of the current agent
  cycle (the same checkpoint as the 90% trigger) — honored even if the
  turn is cancelled.

**Flow:**
```
Host calls agent.compress_context()
    │
    ▼
┌─────────────────────────────────────────┐
│ messages_->compress() called            │
│                                         │
│ 1. Extract first 70% (safe split)       │
│ 2. Convert COPY to plain text           │
│    (originals are NOT deleted yet)      │
│ 3. Send to LLM for summary              │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│ If LLM succeeds:                        │
│ → Delete middle, rebuild with summary   │
│ → messages_ = [system, user(summary), suffix(last 30%)]
│ → outcome = Compressed                  │
│                                         │
│ If LLM fails (all 3 attempts):          │
│ → FALLBACK: delete oldest ~70% anyway   │
│   (context reduced, no summary)         │
│ → outcome = FallbackDeleted             │
│                                         │
│ Both outcomes fire on_compaction() —    │
│ the context changed in both cases.      │
└─────────────────────────────────────────┘
```

### Method 2: Automatic Compression (90% Token Threshold)

**When it triggers:**
- `tokens / max_context_size >= 0.90` (90%)
- Automatically detected after each agent cycle

**Flow:**
```
Agent Cycle Completes
    │
    ▼
┌─────────────────────────────────────────┐
│ Token Check                             │
│                                         │
│ if (messages_->should_compress(tokens)) │
│     → compress() called                 │
│     → First 70% deleted                 │
│     → Summary added if LLM succeeds     │
└─────────────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────────────┐
│ Continue Main Loop                      │
│                                         │
│ iteration_count_ = 0                    │
│ last_total_tokens_ = 0                  │
│ continue;  ← DON'T exit to user input   │
└─────────────────────────────────────────┘
```

---

## The `Messages` Class

**Location:** `src/core/Messages.h` / `Messages.cpp`

### Configuration

```cpp
struct MessageCompressionConfig {
    size_t max_context_size = 1000000;     // API token limit
    float compress_threshold = 0.9f;       // Trigger at 90%
    size_t max_compress_output = 1500;     // Max tokens in summary
    float compress_ratio = 0.70f;          // NEW: Compress first 70%, keep last 30%
    std::string model = "MiniMax-M2.7";
    std::string api_url;
    std::string api_key;
    double temperature = 0.0;              // MUST be pre-clamped by LLMClient via
                                           // ProviderPlugin::clamp_temperature()
                                           // (§BUG-COMPRESS-TEMP — see below)
    int compression_timeout = 30;          // Base seconds; retries use 2x/4x (30/60/120)
    bool debug_output = false;
};
```

### Key Methods

| Method | Purpose |
|--------|---------|
| `should_compress(tokens)` | Check if compression needed (threshold) |
| `compress()` | Main entry point - deletes first 70%, compresses copy |
| `get_compression_indices()` | Returns (start, end) for first 70% with SAFE split |
| `replace_messages(json)` | Replace messages_ with compressed version |

---

## Compression Algorithm Details

### Step 1: Find Safe Split Point at 70%

```cpp
get_compression_indices():
    total_non_system = messages_.size() - 1  // exclude system
    target_split = (total_non_system * 70) / 100  // 70% of non-system
    
    // Find tool_call_ids in target middle range
    first_70_tool_calls = {tool_call_ids from assistants in [1, target_split]}
    
    // Check if any tool result in [1, target_split] references first_70_tool_calls
    // If so, adjust safe_split_point before that tool result
    
    return (1, 1 + safe_split_point)  // start=1, end=exclusive
```

### Step 2: Summarize First, Delete After

```cpp
compress():
    auto [start_idx, end_idx] = get_compression_indices()

    // Make COPY; originals stay intact until the summary succeeds
    json first_70_copy = messages_[start_idx to end_idx)

    // Convert COPY to plain text
    string plain_text = messages_to_plain_text(first_70_copy)

    // Call LLM for summary (3 attempts, growing timeouts:
    // compression_timeout, 2x, 4x — default 30s/60s/120s)
    string summary = call_llm_for_summary(prompt)

    if (summary.empty()) {
        // LAST-RESORT FALLBACK: delete the oldest ~70% anyway so the
        // next API call cannot overflow the context window.
        messages_.erase(oldest ~70%)
        return {FallbackDeleted, {}};  // context reduced WITHOUT a summary
    }

    // Success: delete middle, rebuild as
    // system + user(summary) + last 30%
    ...
    return {Compressed, new_messages.dump()};
```

`compress()` returns a `CompressResult` whose `outcome` distinguishes
four endings (`Compressed`, `NothingToCompress`, `FallbackDeleted`,
`Failed`). Callers fire `on_compaction()` for both `Compressed` and
`FallbackDeleted` — the fallback delete is a real context reduction and
hosts must see it — and do not schedule a retry after a fallback delete.

---

## The Compression API Request (§BUG-COMPRESS-TEMP — read before touching)

`call_llm_for_summary()` builds its **own** request payload and posts it
directly through `HttpSession`. It does **not** pass through
`LLMClient::run_agent_cycle()`, which means none of the provider-plugin
request shaping the main chat path gets (`clamp_temperature`,
`mutate_chat_payload`) applies there automatically.

The invariant that keeps compression working:

> Every field the selected model requires — above all `temperature` —
> must be resolved through the active `ProviderPlugin` **before** it is
> written into `MessageCompressionConfig`. `Messages` is provider-agnostic
> by design and never sees the plugin.

Concretely, `LLMClient` clamps in all three places it populates the
compression config:

- Constructor, settings-loaded path
- Constructor, no-settings fallback path
- `set_config()` on model switch (re-clamped through the *new* plugin —
  a switch can cross providers)

**Why this matters (2026-08-08 incident):** the compression request used
to go out with the model row's raw temperature. Models with a fixed
server-side temperature (Kimi `kimi-for-coding*` / `k3` /
`kimi-k2.7-code` = 1.0) **hard-reject** any other value, so all 3 summary
attempts failed and the last-resort fallback silently deleted ~70% of the
context — compression "worked" only by throwing history away. The
regression test is `tests/test_auto_compress.cpp` → *"compression
request carries provider-clamped temperature"*.

If you add any new LLM call site that hand-builds a payload, apply the
same rule: clamp/mutate through the plugin, or extend the plugin
interface — never send raw model settings.

---

## Compression Prompt

The LLM is given a focused prompt to summarize the first 70%:

```
You are a compression assistant for a coding agent conversation.

Your job is to SUMMARIZE the OLDEST 70% of a conversation. This portion
contains the main work that was done. You need to compress it into a
concise summary.

FIRST 70% OF CONVERSATION TO SUMMARIZE:
[messages converted to plain text]

Requirements:
- Maximum 500 words (~1500 tokens output)
- Focus on: key decisions made, files modified, commands run, errors encountered
- Be concise - this is a transitional summary to bridge the recent 30%
- Output ONLY the summary text, nothing else

Format your output as:
KEY EVENTS: [bullets of what happened]
FILES: [list of files modified]
NEXT: [what needs to happen next]

Start your response with "KEY EVENTS:" and end with "NEXT:" - do not include any
prefix, suffix, or explanation. Just output the summary in the format above.
```

---

## Message Structure

### Before Compression (65 messages)
```
messages = [
    {role: "system", content: "You are a coding agent..."},       // Index 0 (kept)
    {role: "user", content: "Build an app that does X"},           // First 70%
    {role: "assistant", content: "I'll analyze the code..."},     // First 70%
    {role: "tool", content: "File contents..."},                   // First 70%
    {role: "assistant", content: "I see the issue..."},            // First 70%
    {role: "tool", content: "Bash output..."},                      // SAFE SPLIT (here)
    {role: "assistant", content: "Making changes..."},             // Last 30% (kept)
    {role: "tool", content: "Result..."},                            // Last 30% (kept)
    ... (potentially hundreds of messages)
]
```

### After Compression (70/30 Split, ~21 messages)
```
messages = [
    {role: "system", content: "You are a coding agent..."},       // Kept
    {role: "user", content: "[Context compressed — the earlier    // Summary
     conversation was summarized. Summary of the removed messages:
     KEY EVENTS: ...\nFILES: ...\nNEXT: ...
     Continue the current task.]"},
    {role: "assistant", content: "Making changes..."},             // Last 30% (kept)
    {role: "tool", content: "Result..."}                            // Last 30% (kept)
]
```

### Why This Structure?

The summary rides inside a synthetic leading **user** message:
1. Strict chat templates (e.g. qwen3's jinja) require the first message
   after `system` to be a `user` message — assistant/tool-first
   histories are rejected.
2. No assistant/tool pair is emitted, so the rebuilt chain stays valid
   for every provider regardless of tool-call chaining rules.
3. The summary remains in a queryable, debuggable position at the head
   of the history.

---

## Token Management

| Setting | Default | Description |
|---------|---------|-------------|
| `compress_threshold` | 0.9 | Trigger auto-compress at 90% |
| `compress_ratio` | 0.70 | Compress first 70%, keep last 30% |
| `max_compress_output` | 1500 | Max tokens in LLM summary |
| `compression_timeout` | 30 | Base seconds per LLM attempt; 3 attempts use base, 2x, 4x (default 30s/60s/120s) |

### Per-Model Context Size

Each model in the `daedalus::SettingsStore` can specify its own
`max_context_size`. This is the only way to set context size (no
global setting). See [`docs/settings.md`](settings.md) for the
`upsert_model()` API.

```cpp
#include <daedalus/settings.h>
auto& store = daedalus::SettingsStore::instance();
store.upsert_model(daedalus::ModelConfig{
    .name             = "minimax-m3",
    .host             = "https://api.minimax.io",
    .model_id         = "MiniMax-M3",
    .max_context_size = 500000,
});
```

When you set a model as default, daedalus uses that model's
`max_context_size` for the 90% compression trigger. If a model
doesn't specify `max_context_size`, a code default of 1,000,000 is used.

### Token Estimation

daedalus estimates token count using:
- General text: ~4 characters per token (rough estimate)

---

## Debug Files

Compression generates debug files in `/tmp/` for troubleshooting:

| File | Content |
|------|---------|
| `/tmp/daedalus_compress_middle.txt` | First 70% messages being compressed |

---

## Integration with Agent Loop

The compression happens in `LLMClient`:

```cpp
// In run_one_turn(), after each agent cycle:
bool at_threshold = messages_->should_compress(last_total_tokens_);
if (at_threshold || host_requested) {
    const size_t before_tokens = last_total_tokens_;
    messages_->set_messages(messages);
    auto result = messages_->compress();
    if (result.outcome == CompressOutcome::Compressed) {
        messages_->replace_messages(result.compressed_json);
        messages = messages_->get_messages();
        sink_->on_compaction(before_tokens, messages_->estimate_tokens());
    } else if (result.outcome == CompressOutcome::FallbackDeleted) {
        // Context reduced WITHOUT a summary — still fires on_compaction.
        messages = messages_->get_messages();
        sink_->on_compaction(before_tokens, messages_->estimate_tokens());
    }
    iteration_count_ = 0;
    last_total_tokens_ = 0;
    continue;  // DON'T exit to user input - continue main loop!
}
```

### Critical: Don't Exit to User Input

After compression (success or failure), the main loop MUST continue:
- `continue;` statement keeps agent running
- `iteration_count_` reset to 0
- `last_total_tokens_` reset to 0
- Agent resumes with reduced context

If LLM fails during compression:
- The fallback deletes the oldest ~70% (context reduced, no summary)
- `compress()` returns `CompressOutcome::FallbackDeleted`
- `on_compaction(before, after)` still fires — hosts see the reduction
- No retry is scheduled for a host-requested compress
- `continue;` still executes - main loop continues
- No exit to user input

---

## Key Differences: Host-Triggered vs Automatic

| Aspect | `compress_context()` | Auto (90%) |
|--------|----------------------|------------|
| **Trigger** | Host API call | Token threshold |
| **Continuation** | Continue main loop | Continue main loop |
| **Message** | "Context compressed..." | "Context compressed..." |
| **Use case** | Fresh start | Prevent context overflow |

Both methods follow the same flow:
1. Copy the first 70% (at safe split point), summarize via LLM
2. On success, delete the middle and rebuild with the summary
3. On failure, fall back to deleting the oldest ~70% without a summary
4. Both reductions fire `on_compaction()`
5. Continue main loop (don't exit)

---

## Error Handling

### LLM Failure During Compression

If `call_llm_for_summary()` returns empty string after all 3 attempts:
1. The fallback deletes the oldest ~70% of `messages_` (last resort to
   prevent context overflow)
2. Context is reduced (even without summary)
3. `compress()` returns `CompressOutcome::FallbackDeleted`
4. The caller fires `on_compaction(before, after)` so host stats/UI
   reflect the reduction, and logs the fallback
5. A host-requested compress is NOT re-armed after a fallback delete
6. Main loop continues via `continue;`

If the history is too small to safely reduce (≤5 messages), `compress()`
returns `CompressOutcome::Failed` instead and the context is left
untouched.

The context is smaller even without a summary, which is acceptable per requirements.

### JSON Parse Error

If `replace_messages()` fails to parse the compressed JSON:
1. Log error: "[Messages] Failed to replace messages: {e.what()}"
2. `messages_` remains in deleted state (smaller context)
3. Main loop continues with reduced context

---

## Testing

Run message tests to verify compression:

```bash
./build/tests/daedalus_messages_tests
```

Key test cases:
- `split_messages` - verifies 70/30 split with safe point
- `should_compress` - verifies threshold detection
- `compress` - verifies full compression flow
- `safe_split_point` - verifies split doesn't break tool call chains
- `compression request carries provider-clamped temperature`
  (`tests/test_auto_compress.cpp`) - §BUG-COMPRESS-TEMP regression:
  proves the summary request goes out with the plugin-clamped
  temperature the selected model requires, not the raw store value
