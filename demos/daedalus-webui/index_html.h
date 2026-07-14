// SPDX-License-Identifier: BSL-1.0
// demos/daedalus-webui/index_html.h
//
// Browser UI for the webui demo. Self-contained vanilla DOM + EventSource.
// Extracted from main.cpp to keep main.cpp under 300 lines.

#pragma once

namespace websse {

inline constexpr const char* kIndexHtml = R"HTML(<html lang="en">
<head>
<meta charset="utf-8">
<title>daedalus-webui</title>
<style>
  :root { color-scheme: dark; }
  body {
    margin: 0; padding: 12px;
    background: #0f1115; color: #d6d9dc;
    font: 14px/1.4 ui-monospace, Menlo, Consolas, monospace;
  }
  header {
    display: flex; align-items: center; gap: 12px;
    padding: 8px 12px; border-bottom: 1px solid #2a2f36;
    margin-bottom: 12px;
  }
  header h1 { margin: 0; font-size: 16px; font-weight: 600; }
  #state {
    padding: 2px 10px; border-radius: 999px; font-size: 12px;
    background: #2a2f36; color: #d6d9dc;
  }
  #state.running  { background: #1f6feb; }
  #state.done     { background: #2ea043; }
  #state.cancelled, #state.error { background: #f85149; }
  main { display: grid; grid-template-columns: 2fr 1fr 1fr; gap: 12px;
         height: calc(100vh - 90px); }
  section { display: flex; flex-direction: column;
            background: #161b22; border: 1px solid #2a2f36;
            border-radius: 6px; overflow: hidden; }
  section h2 { margin: 0; padding: 8px 12px; font-size: 12px;
               font-weight: 600; color: #8b949e;
               border-bottom: 1px solid #2a2f36; text-transform: uppercase;
               letter-spacing: 0.5px; }
  .body { flex: 1; overflow-y: auto; padding: 10px 12px; }
  .body p  { margin: 0 0 8px; white-space: pre-wrap; }
  .body .delta { color: #d6d9dc; }
  .body .log   { color: #8b949e; font-size: 12px; }
  .body .warn  { color: #d29922; }
  .body .error { color: #f85149; }
  .body .tool-call   { color: #58a6ff; margin-top: 6px; }
  .body .tool-result { color: #8b949e; margin: 2px 0 8px 12px; font-size: 12px; }
  form { display: flex; gap: 8px; padding: 8px 12px;
         border-top: 1px solid #2a2f36; background: #0f1115; }
  input[type=text] { flex: 1; padding: 6px 10px; border: 1px solid #2a2f36;
                     border-radius: 4px; background: #161b22; color: #d6d9dc;
                     font: inherit; }
  input[type=text]:focus { outline: none; border-color: #1f6feb; }
  button { padding: 6px 14px; background: #238636; color: #fff;
           border: none; border-radius: 4px; font: inherit; cursor: pointer; }
  button.cancel { background: #6e7681; }
  button:disabled { opacity: 0.5; cursor: not-allowed; }
  .empty { color: #6e7681; font-style: italic; }
  /* Right-side header group: holds the model name + cwd label.
     margin-left:auto pins the whole group to the right edge. */
  #right-info {
    margin-left: auto;
    display: flex; align-items: center; gap: 14px;
    min-width: 0;             /* allow children to shrink/truncate */
  }
  .info-pill {
    font-size: 12px; color: #8b949e;
    max-width: 32ch; overflow: hidden;
    text-overflow: ellipsis; white-space: nowrap;
    direction: rtl;           /* show the tail (e.g. path basename) */
    text-align: left;
  }
  #model::before { content: "model: "; color: #6e7681; }
  #cwd::before   { content: "cwd: ";   color: #6e7681; }

  /* === Phase 9.10: thinking-bubble + stage-chips =================== */

  /* §Phase 9.10: bouncing-dots "thinking" bubble that appears in the
     transcript the moment the user submits and disappears when the
     first assistant_delta arrives. CSS-only animation (no JS timer). */
  #thinking-bubble {
    display: inline-flex; align-items: center; gap: 5px;
    padding: 8px 14px; margin: 4px 0 8px;
    background: #1f2937; color: #d6d9dc;
    border-radius: 14px; width: fit-content;
    animation: bubble-fade-in 180ms ease-out;
  }
  #thinking-bubble .dot {
    width: 7px; height: 7px; border-radius: 50%;
    background: #8b949e;
    animation: dot-bounce 1.2s infinite ease-in-out;
  }
  /* Stagger the three dots so they bounce sequentially, like iMessage. */
  #thinking-bubble .dot:nth-child(1) { animation-delay: 0s;    }
  #thinking-bubble .dot:nth-child(2) { animation-delay: 0.15s; }
  #thinking-bubble .dot:nth-child(3) { animation-delay: 0.30s; }
  #thinking-bubble .label {
    margin-left: 4px; font-size: 12px; color: #8b949e;
  }
  @keyframes dot-bounce {
    0%, 60%, 100% { transform: translateY(0);    opacity: 0.4; }
    30%           { transform: translateY(-5px); opacity: 1;   }
  }
  @keyframes bubble-fade-in {
    from { opacity: 0; transform: translateY(4px); }
    to   { opacity: 1; transform: translateY(0);   }
  }

  /* §Phase 9.10: stage chip row above the input. Shows the agent's
     current planning phase (Understanding → Planning → Acting →
     Verifying) with the active stage pulsing. Like Claude Code /
     ChatGPT, gives the user a sense of *what* the agent is doing. */
  #stage-chips {
    display: flex; gap: 6px;
    padding: 6px 12px;
    border-top: 1px solid #2a2f36;
    background: #0f1115;
    overflow-x: auto;
  }
  .stage-chip {
    padding: 3px 10px; border-radius: 999px;
    font-size: 11px; line-height: 1.4;
    background: #161b22; color: #6e7681;
    border: 1px solid #2a2f36;
    transition: background 200ms, color 200ms, border-color 200ms;
    white-space: nowrap;
  }
  .stage-chip.active {
    background: #1f6feb; color: #fff; border-color: #1f6feb;
    animation: chip-pulse 1.6s infinite ease-in-out;
  }
  .stage-chip.done {
    background: #0d4429; color: #56d364; border-color: #2ea043;
  }
  @keyframes chip-pulse {
    0%, 100% { box-shadow: 0 0 0 0 rgba(31, 111, 235, 0.5); }
    50%      { box-shadow: 0 0 0 6px rgba(31, 111, 235, 0);  }
  }
  /* §Phase 9.12: terminal state chips (Done / Cancelled / Error) — solid
     green / amber / red so the agent's final outcome is obvious even if
     the rest of the UI has scrolled. */
  .stage-chip.terminal {
    margin-left: auto;  /* push to the right of the row */
  }
  .stage-chip.terminal[data-stage="Done"] {
    background: #0d4429; color: #56d364; border-color: #2ea043;
  }
  .stage-chip.terminal[data-stage="Cancelled"] {
    background: #4a3500; color: #d29922; border-color: #9e6a03;
  }
  .stage-chip.terminal[data-stage="Error"] {
    background: #5a1e1e; color: #f85149; border-color: #da3633;
  }
  /* §Phase 9.12: state pill in the header gets a coloured bg too so the
     "Done" indication is impossible to miss. */
  #state {
    padding: 2px 10px; border-radius: 999px;
    background: #161b22; color: #6e7681;
    border: 1px solid #2a2f36;
    font-size: 12px; font-weight: 600;
    transition: background 200ms, color 200ms, border-color 200ms;
  }
  #state.thinking {
    background: #1f6feb22; color: #58a6ff; border-color: #1f6feb;
  }
  #state.done {
    background: #2ea04322; color: #56d364; border-color: #2ea043;
  }
  #state.cancelled {
    background: #9e6a0322; color: #d29922; border-color: #9e6a03;
  }
  #state.error {
    background: #da363322; color: #f85149; border-color: #da3633;
  }
</style>
</head>
<body>
<header>
  <h1>daedalus-webui</h1>
  <span id="state">Idle</span>
  <span id="stats" class="empty">no session yet</span>
  <div id="right-info">
    <span id="model" title="" class="empty info-pill">…</span>
    <span id="cwd"   title="" class="empty info-pill">…</span>
  </div>
</header>
<main>
  <section>
    <h2>Transcript</h2>
    <div id="transcript" class="body"></div>
    <!-- §Phase 9.10: stage chip row. Above the input so it's always
         visible, regardless of scroll position. -->
    <div id="stage-chips" hidden>
      <span class="stage-chip" data-stage="Understanding">Understanding</span>
      <span class="stage-chip" data-stage="Planning">Planning</span>
      <span class="stage-chip" data-stage="Acting">Acting</span>
      <span class="stage-chip" data-stage="Verifying">Verifying</span>
      <span class="stage-chip terminal" data-stage="Done">✓ Done</span>
    </div>
    <form id="composer">
      <input type="text" id="input" autocomplete="off"
             placeholder="Send a message..." disabled>
      <button type="submit" id="send" disabled>Send</button>
      <button type="button" id="cancel" disabled>Cancel</button>
    </form>
  </section>
  <section>
    <h2>Tool calls</h2>
    <div id="tools" class="body"><span class="empty">No tool calls yet.</span></div>
  </section>
  <section>
    <h2>Stats / log</h2>
    <div id="log" class="body"><span class="empty">No events yet.</span></div>
  </section>
</main>

<script>
"use strict";

const $ = (id) => document.getElementById(id);
const transcriptEl = $("transcript");
const toolsEl      = $("tools");
const logEl        = $("log");
const stateEl      = $("state");
const statsEl      = $("stats");
const inputEl      = $("input");
const sendBtn      = $("send");
const cancelBtn    = $("cancel");

let sessionId = null;
let es = null;
let currentDelta = null;

// §Phase 9.5: expose handle on window for debugging (devtools, BiDi tests).
// This is a tiny one-line change that makes "what session am I in?"
// and "is the SSE connected?" trivially observable from any browser
// automation harness.
Object.defineProperty(window, "sessionId", {
    get: () => sessionId,
    set: v => { sessionId = v; }
});
Object.defineProperty(window, "es", {
    get: () => es,
    set: v => { es = v; }
});

function append(el, html, cls) {
  const p = document.createElement("p");
  if (cls) p.className = cls;
  p.innerHTML = html;
  el.appendChild(p);
  el.scrollTop = el.scrollHeight;
  return p;
}
function escapeHtml(s) {
  return s.replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c]));
}
function appendLog(level, msg) {
  if (logEl.querySelector(".empty")) logEl.innerHTML = "";
  const cls = level === "Warn"  ? "warn"
            : level === "Error" ? "error" : "log";
  return append(logEl, "[" + level + "] " + escapeHtml(msg), cls);
}
function setState(name) {
  stateEl.textContent = name;
  // §Phase 9.12: derive a coloured class from the state so the header
  // pill is hard to miss when the agent finishes.
  const stateClass = (name === "Done")     ? "done"
                   : (name === "Cancelled") ? "cancelled"
                   : (name === "Error")    ? "error"
                   : (name === "Running" || name === "Planning" ||
                      name === "Acting"   || name === "Verifying" ||
                      name === "Understanding" || name === "Compressing")
                                          ? "thinking"
                   : "";
  stateEl.className = stateClass;
  // §Phase 9.5: remember the last known state so we can restore it
  // when the SSE connection reconnects (the agent didn't change state,
  // only the network blipped).
  stateEl.dataset.lastName = name;
  const running = (name === "Running" || name === "Planning" ||
                   name === "Acting"   || name === "Verifying" ||
                   name === "Understanding" || name === "Compressing");
  sendBtn.disabled   = !sessionId || running;
  cancelBtn.disabled = !sessionId || !running;
  inputEl.disabled   = !sessionId || running;
}

// === Phase 9.10: thinking bubble + stage chips =====================
//
// Two cooperating indicators driven by the same SSE state_change event:
//   - Bouncing-dots bubble in the transcript pane.
//   - Multi-stage chip row above the input.
// Together they cover both peripheral and central feedback:
//   - Peripheral: chip row pulses on the active stage.
//   - Central:    bubble in the transcript shows *something is happening
//                 HERE* and disappears the moment the first token
//                 arrives (auto-stop on assistant_delta).
//
// Both reset to "hidden" on terminal states (Done, Error, Cancelled,
// AwaitingApproval).

// Stages that warrant showing the thinking bubble + chip row.
// Anything not in this list (e.g. Done) hides both.
const THINKING_STATES = new Set([
  "Understanding", "Planning", "Acting", "Verifying", "Compressing",
]);

// §Phase 9.10: show bouncing-dots "Thinking…" bubble in the transcript.
function showThinkingBubble() {
  if (document.getElementById("thinking-bubble")) return;  // already shown
  const bubble = document.createElement("div");
  bubble.id = "thinking-bubble";
  bubble.innerHTML =
    '<span class="dot"></span>' +
    '<span class="dot"></span>' +
    '<span class="dot"></span>' +
    '<span class="label">Thinking…</span>';
  transcriptEl.appendChild(bubble);
  transcriptEl.scrollTop = transcriptEl.scrollHeight;
}

function hideThinkingBubble() {
  const bubble = document.getElementById("thinking-bubble");
  if (bubble) bubble.remove();
}

// §Phase 9.10: update the multi-stage chip row.
//   - First time we see a "thinking" state, reveal the row.
//   - The current stage gets `.active` (blue, pulsing).
//   - Earlier stages in the pipeline get `.done` (green).
//   - Later stages stay unstyled (grey, "pending").
//   - §Phase 9.12: terminal states (Done / Cancelled / Error) also keep
//     the row visible so the user sees the green ✓ Done chip — that's
//     the "agent finished" cue.  We mark every prior chip as `.done`
//     and the terminal one as `.active` so it pulses briefly.
function setStage(name) {
  const chips = document.getElementById("stage-chips");
  if (!chips) return;
  const order = ["Understanding", "Planning", "Acting", "Verifying"];
  const isTerminal = (name === "Done" || name === "Cancelled" || name === "Error");
  if (!THINKING_STATES.has(name) && !isTerminal) {
    // Truly idle (e.g. agent hasn't started yet) — hide the row.
    chips.hidden = true;
    return;
  }
  chips.hidden = false;
  // Pipeline order matters — earlier stages become "done".
  // Compressing isn't in the chip list (it's a side-rail action),
  // so treat it as "no chip change".
  const currentIdx = order.indexOf(name);
  for (const chip of chips.querySelectorAll(".stage-chip")) {
    const stage = chip.dataset.stage;
    const idx = order.indexOf(stage);
    chip.classList.remove("active", "done");
    if (currentIdx < 0 && !isTerminal) continue;
    if (stage === name)        chip.classList.add("active");
    else if (idx >= 0 && idx < currentIdx) chip.classList.add("done");
    else if (isTerminal && idx >= 0)        chip.classList.add("done");
  }
}

function newSession() {
  if (es) { es.close(); es = null; }
  transcriptEl.innerHTML = "";
  toolsEl.innerHTML      = '<span class="empty">No tool calls yet.</span>';
  logEl.innerHTML        = '<span class="empty">No events yet.</span>';
  statsEl.textContent     = "creating session...";
  fetch("/sessions", { method: "POST" })
    .then(r => r.json())
    .then(j => {
      sessionId = j.session_id;
      statsEl.textContent = "session " + sessionId.slice(0, 8) + "...";

      // §Phase 9.6: surface the active model in the header. The
      // server resolves it from cfg.model_name / settings.json
      // defaults at session creation time, so it's stable for the
      // session's lifetime unless we add a live model-switch UI.
      const modelEl = document.getElementById("model");
      if (j.model && (j.model.name || j.model.id)) {
        const display = j.model.name || j.model.id;
        const tip = j.model.id && j.model.name !== j.model.id
                      ? j.model.name + " (" + j.model.id + ")"
                      : (j.model.host ? j.model.host + " / " + display : display);
        modelEl.textContent = display;
        modelEl.title = tip;
        modelEl.classList.remove("empty");
      }

      openEventStream();
      inputEl.disabled = false;
      sendBtn.disabled = false;
      inputEl.focus();
    })
    .catch(e => appendLog("Error", "create session failed: " + e));
}

function openEventStream() {
  // §Phase 9.5 (2026-07-10): track reconnect state so the UI doesn't
  // flood with "SSE connection dropped; refresh to retry" warnings on
  // every EventSource auto-reconnect. EventSource auto-reconnects
  // silently by default — the previous onerror handler logged a warning
  // each time, making the panel look broken when it wasn't.
  //
  // New behaviour:
  //   - First error        → log "SSE disconnected, reconnecting…" once
  //   - 'open' fires again → log "SSE reconnected"
  //   - Many failures in a row → only after 5+ consecutive failures,
  //                              escalate to "refresh to retry"
  //   - The status pill also flips to yellow while disconnected.
  let disconnectCount = 0;
  es = new EventSource("/sessions/" + sessionId + "/events");

  es.onopen = () => {
    if (disconnectCount > 0) {
      appendLog("Info", "SSE reconnected.");
    }
    disconnectCount = 0;
    setState(document.getElementById("state").dataset.lastName || "Idle");
  };
  es.onerror = () => {
    ++disconnectCount;
    if (disconnectCount === 1) {
      appendLog("Info", "SSE disconnected, reconnecting…");
    } else if (disconnectCount === 5) {
      appendLog("Warn", "SSE has failed " + disconnectCount +
                         " times; the connection is unstable. " +
                         "Refresh the page if it doesn't recover.");
    } else if (disconnectCount > 5 && disconnectCount % 10 === 0) {
      // Don't spam — only every 10 failures after the first warning.
      appendLog("Warn", "SSE still failing (" + disconnectCount +
                         " attempts so far).");
    }
    // Note: don't call es.close() — let EventSource auto-reconnect.
  };

  es.addEventListener("state_change", (e) => {
    const to = JSON.parse(e.data).to;
    setState(to);
    // §Phase 9.10: drive the thinking bubble + stage chips from
    // the same state_change event. Bubble auto-shows on entry to
    // a thinking state, auto-hides on entry to a terminal state
    // (Done / Error / Cancelled / Awaiting*). The first
    // assistant_delta will also hide it on the streaming path.
    if (THINKING_STATES.has(to)) {
      setStage(to);
      showThinkingBubble();
    } else {
      setStage(to);   // also hides the chip row
      hideThinkingBubble();
    }
  });
  es.addEventListener("assistant_delta", (e) => {
    // §Phase 9.10: hide the thinking bubble the moment real text
    // starts streaming. The bubble would otherwise compete with
    // the text for the user's attention (and flicker in/out as
    // the scroll moves).
    hideThinkingBubble();
    const d = JSON.parse(e.data);
    if (!currentDelta) currentDelta = append(transcriptEl, "", "delta");
    currentDelta.textContent = (currentDelta.textContent || "") + d.text;
    transcriptEl.scrollTop = transcriptEl.scrollHeight;
  });
  es.addEventListener("assistant_end", () => { currentDelta = null; });
  es.addEventListener("tool_call", (e) => {
    if (toolsEl.querySelector(".empty")) toolsEl.innerHTML = "";
    const d = JSON.parse(e.data);
    append(toolsEl, "→ " + escapeHtml(d.name) + " " +
                     escapeHtml(JSON.stringify(d.args)), "tool-call");
  });
  es.addEventListener("tool_result", (e) => {
    const d = JSON.parse(e.data);
    append(toolsEl, "← " + escapeHtml(JSON.stringify(d.result).slice(0, 400)),
                      "tool-result");
  });
  es.addEventListener("log", (e) => {
    const d = JSON.parse(e.data);
    appendLog(d.level, d.message);
  });
}

document.getElementById("composer").addEventListener("submit", (ev) => {
  ev.preventDefault();
  const text = inputEl.value.trim();
  if (!text || !sessionId) return;
  inputEl.value = "";
  append(transcriptEl, "› " + escapeHtml(text), "log");
  // §Phase 9.10: clean up any leftover indicator from a previous turn
  // (the chip row was hidden on the previous terminal state, but be
  // defensive). The next state_change event will re-show them.
  hideThinkingBubble();
  setStage("Idle");
  fetch("/sessions/" + sessionId + "/messages", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ message: text }),
  })
    .then(r => r.json())
    .then(j => { if (j.error) appendLog("Error", j.error); })
    .catch(e => appendLog("Error", "send failed: " + e));
});

cancelBtn.addEventListener("click", () => {
  if (!sessionId) return;
  fetch("/sessions/" + sessionId + "/cancel", { method: "POST" })
    .catch(e => appendLog("Error", "cancel failed: " + e));
});

setState("Idle");
newSession();

// §Phase 9.6: populate the top-right "model:" and "cwd:" labels.
//  - cwd  : fetched from GET /info (server-wide, captured at startup).
//  - model: returned by POST /sessions inside the session JSON (the
//           Agent.active_model() resolves to whichever entry is the
//           default at session creation time; if the user later adds
//           a /sessions/<id>/model PATCH we'll update this live).
// Both are best-effort — leave the "…" placeholder on any failure.
fetch("/info").then(r => r.json()).then(j => {
    const cwdEl = document.getElementById("cwd");
    if (j && typeof j.cwd === "string" && j.cwd.length > 0) {
        cwdEl.textContent = j.cwd;
        cwdEl.title = j.cwd;
        cwdEl.classList.remove("empty");
    }
}).catch(() => { /* leave the placeholder */ });
</script>
</body>
</html>
)HTML";

}
