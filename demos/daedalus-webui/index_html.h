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
  form { display: flex; gap: 8px; padding: 8px 12px; align-items: flex-end;
         border-top: 1px solid #2a2f36; background: #0f1115; }
  textarea#input { flex: 1; padding: 6px 10px; border: 1px solid #2a2f36;
                   border-radius: 4px; background: #161b22; color: #d6d9dc;
                   font: inherit; resize: none; min-height: 20px;
                   max-height: 200px; field-sizing: content; }
  textarea#input:focus { outline: none; border-color: #1f6feb; }
  button { padding: 6px 14px; background: #238636; color: #fff;
           border: none; border-radius: 4px; font: inherit; cursor: pointer; }
  button.cancel { background: #6e7681; }
  button.deny { background: #da3633; }
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

  /* === Phase D1: Tool Calls panel — structured cards ====================
     Each tool invocation gets its own card, keyed by the OpenAI-style
     `call_id` emitted by the SSE sink. Card state transitions:
        running  → has progress bar, no result yet
        success  → green check, result rendered (collapsed by default)
        error    → red x, error message rendered
     Progress events from BashTool etc. drive the bar in real time. */

  .tool-card {
    margin: 0 0 8px;
    padding: 8px 10px;
    background: #0d1117;
    border: 1px solid #2a2f36;
    border-radius: 6px;
    transition: border-color 200ms, background 200ms;
  }
  .tool-card.running { border-color: #1f6feb; background: #0d1117; }
  .tool-card.success { border-color: #2ea043; }
  .tool-card.error   { border-color: #da3633; }
  .tool-card-head {
    display: flex; align-items: center; gap: 8px;
    font-size: 13px; font-weight: 600;
  }
  .tool-card-name  { color: #58a6ff; flex: 1;
                     overflow: hidden; text-overflow: ellipsis;
                     white-space: nowrap; }
  .tool-card-icon  {
    display: inline-block; width: 16px; text-align: center;
    color: #6e7681;
  }
  .tool-card.running .tool-card-icon { color: #58a6ff;
                                      animation: spin 1.2s linear infinite; }
  .tool-card.success .tool-card-icon { color: #56d364; }
  .tool-card.error   .tool-card-icon { color: #f85149; }
  @keyframes spin { from { transform: rotate(0deg); }
                    to   { transform: rotate(360deg); } }
  .tool-card-stage {
    font-size: 11px; color: #8b949e;
    padding: 2px 6px; border-radius: 999px;
    background: #161b22; border: 1px solid #2a2f36;
  }
  .tool-card.running .tool-card-stage { color: #58a6ff;
                                        border-color: #1f6feb; }
  .tool-card-args {
    margin-top: 6px; font-size: 11px; color: #8b949e;
    font-family: ui-monospace, Menlo, Consolas, monospace;
    white-space: pre-wrap; word-break: break-all;
    max-height: 80px; overflow-y: auto;
  }
  .tool-card-progress {
    margin-top: 6px; height: 4px;
    background: #161b22; border-radius: 2px;
    overflow: hidden;
    /* Hidden when no progress bar is active — JS toggles display. */
    display: none;
  }
  .tool-card.running .tool-card-progress { display: block; }
  .tool-card-progress-bar {
    height: 100%; width: 0%;
    background: linear-gradient(90deg, #1f6feb, #58a6ff);
    transition: width 300ms ease-out;
  }
  /* Indeterminate "shimmer" while no percent is known — bar slides
     left-to-right without filling. */
  .tool-card.running.indeterminate .tool-card-progress-bar {
    width: 30%;
    animation: shimmer 1.4s infinite ease-in-out;
  }
  @keyframes shimmer {
    0%   { transform: translateX(-100%); }
    100% { transform: translateX(400%); }
  }
  .tool-card-message {
    margin-top: 4px; font-size: 11px; color: #8b949e;
    font-style: italic;
  }
  .tool-card-result {
    margin-top: 6px; font-size: 11px;
    color: #8b949e;
    font-family: ui-monospace, Menlo, Consolas, monospace;
    background: #161b22; padding: 4px 6px;
    border-radius: 4px;
    max-height: 120px; overflow-y: auto;
    white-space: pre-wrap; word-break: break-all;
  }
  .tool-card.error .tool-card-result { color: #f85149; }
  /* §Phase D2.1: tool-call counter badge in the section header.
     Lives inside <h2>, so overrides the inherited uppercase +
     letter-spacing. Shows "1 pending · 2 ok · 1 err" — only the
     non-zero counts. Hidden until at least one tool call has
     happened in this session. */
  .tool-counter {
    font-size: 11px; font-weight: 500;
    padding: 1px 8px;
    border-radius: 999px;
    background: #161b22; color: #8b949e;
    border: 1px solid #2a2f36;
    margin-left: 6px;
    text-transform: none; letter-spacing: normal;
    transition: color 200ms, border-color 200ms, background 200ms;
    cursor: default;
  }
  .tool-counter.has-pending { color: #58a6ff; border-color: #1f6feb;
                              background: #1f6feb22; }
  .tool-counter.has-error   { color: #f85149; border-color: #da3633;
                              background: #da363322; }

  /* §Phase D2.2: per-card collapse toggle on long results.
     Default (.tool-card-result without .expanded) already has
     max-height: 120px from the base rule above. .expanded bumps
     to 800px so very long output doesn't dominate the page but
     the user can read most of it without an internal scroll. */
  .tool-card-result.expanded {
    max-height: 800px;
  }
  .tool-card-collapse {
    display: inline-block; margin: 4px 0 0;
    padding: 2px 8px;
    font-size: 11px;
    background: transparent;
    color: #58a6ff;
    border: 1px solid #1f6feb;
    border-radius: 4px;
    cursor: pointer;
    font-family: inherit;
  }
  .tool-card-collapse:hover { background: #1f6feb22; }

  /* §webui-approval: modal dialog for bash command approval */
  #approval-modal {
    position: fixed; inset: 0; z-index: 100;
    background: rgba(0,0,0,0.7);
    display: none; align-items: center; justify-content: center;
  }
  #approval-modal.visible { display: flex; }
  .approval-box {
    background: #161b22; border: 1px solid #2a2f36;
    border-radius: 8px; padding: 16px; max-width: 600px; width: 90%;
    box-shadow: 0 10px 30px rgba(0,0,0,0.4);
  }
  .approval-box h3 { margin: 0 0 10px; font-size: 14px; color: #d6d9dc; }
  .approval-box pre {
    background: #0d1117; border: 1px solid #2a2f36; border-radius: 4px;
    padding: 10px; overflow-x: auto; white-space: pre-wrap; word-break: break-all;
    font: 12px/1.4 ui-monospace, Menlo, Consolas, monospace;
    color: #d6d9dc; max-height: 200px;
  }
  .approval-box .buttons {
    display: flex; gap: 8px; justify-content: flex-end; margin-top: 12px;
  }

  /* §webui-usage: compact token/iteration HUD in the header */
  #usage-hud {
    font-size: 11px; color: #8b949e;
    display: flex; gap: 10px; align-items: center;
    padding: 2px 8px; border-radius: 4px;
    background: #161b22; border: 1px solid #2a2f36;
    white-space: nowrap;
  }
  #usage-hud .bar {
    width: 60px; height: 6px; background: #2a2f36; border-radius: 3px;
    overflow: hidden; display: inline-block; vertical-align: middle;
  }
  #usage-hud .bar-fill {
    height: 100%; background: #1f6feb; width: 0%;
    transition: width 200ms ease-out;
  }
  #usage-hud .bar-fill.warn { background: #d29922; }
  #usage-hud .bar-fill.danger { background: #f85149; }
</style>
</head>
<body>
<header>
  <h1>daedalus-webui</h1>
  <span id="state">Idle</span>
  <span id="stats" class="empty">no session yet</span>
  <span id="usage-hud" title="Tokens and iterations"></span>
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
      <textarea id="input" autocomplete="off" rows="1"
                placeholder="Send a message... (Ctrl+Enter)" disabled></textarea>
      <button type="submit" id="send" disabled>Send</button>
      <button type="button" id="cancel" disabled>Cancel</button>
    </form>
  </section>
  <section>
    <h2>Tool calls <span id="tool-counter" class="tool-counter" hidden></span></h2>
    <div id="tools" class="body"><span class="empty">No tool calls yet.</span></div>
  </section>
  <section>
    <h2>Stats / log</h2>
    <div id="log" class="body"><span class="empty">No events yet.</span></div>
  </section>
</main>

<!-- §webui-approval: bash command approval modal -->
<div id="approval-modal">
  <div class="approval-box">
    <h3>Approve bash command?</h3>
    <pre id="approval-command"></pre>
    <div class="buttons">
      <button type="button" id="approval-deny" class="deny">Deny</button>
      <button type="button" id="approval-approve">Approve</button>
    </div>
  </div>
</div>

<script>
"use strict";

const $ = (id) => document.getElementById(id);
const transcriptEl = $("transcript");
const toolsEl      = $("tools");
const logEl        = $("log");
const stateEl      = $("state");
const statsEl      = $("stats");
const usageHudEl   = $("usage-hud");
const inputEl      = $("input");
const sendBtn      = $("send");
const cancelBtn    = $("cancel");
const approvalModal = $("approval-modal");
const approvalCommandEl = $("approval-command");
const approvalApproveBtn = $("approval-approve");
const approvalDenyBtn    = $("approval-deny");

let sessionId = null;
let es = null;
let currentDelta = null;
let statsInterval = null;

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

// §webui-usage: poll /sessions/<id>/stats and render a compact HUD.
function renderUsageHud(st) {
  if (!st || st.error) {
    usageHudEl.textContent = "";
    return;
  }
  const pct = st.max_context_size > 0
              ? Math.min(100, Math.round(st.total_tokens / st.max_context_size * 100))
              : 0;
  const iterPct = st.max_iterations > 0
                  ? Math.min(100, Math.round(st.iteration / st.max_iterations * 100))
                  : 0;
  const barClass = pct >= 95 ? "danger" : pct >= 80 ? "warn" : "";
  usageHudEl.innerHTML =
    '<span title="tokens">tk ' + st.total_tokens + '/' + st.max_context_size +
    ' (' + pct + '%)</span>' +
    '<span class="bar"><span class="bar-fill ' + barClass +
    '" style="width:' + pct + '%"></span></span>' +
    '<span title="iterations">it ' + st.iteration + '/' + st.max_iterations +
    ' (' + iterPct + '%)</span>';
}
function startStatsPolling() {
  if (statsInterval) clearInterval(statsInterval);
  statsInterval = setInterval(() => {
    if (!sessionId) return;
    fetch("/sessions/" + sessionId + "/stats")
      .then(r => r.json())
      .then(renderUsageHud)
      .catch(() => { /* ignore transient poll failures */ });
  }, 2000);
}
function stopStatsPolling() {
  if (statsInterval) { clearInterval(statsInterval); statsInterval = null; }
}

// §webui-approval: show/hide the bash approval modal.
function showApproval(command) {
  approvalCommandEl.textContent = command;
  approvalModal.classList.add("visible");
}
function hideApproval() {
  approvalModal.classList.remove("visible");
  approvalCommandEl.textContent = "";
}
function sendApproval(approved) {
  if (!sessionId) return;
  fetch("/sessions/" + sessionId + "/approve", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ approved: approved, sticky: false }),
  }).catch(e => appendLog("Error", "approval failed: " + e));
  hideApproval();
}
approvalApproveBtn.addEventListener("click", () => sendApproval(true));
approvalDenyBtn.addEventListener("click", () => sendApproval(false));

// §webui-textarea: auto-resize and keyboard shortcuts.
inputEl.addEventListener("input", () => {
  inputEl.style.height = "auto";
  inputEl.style.height = Math.min(200, inputEl.scrollHeight) + "px";
});
inputEl.addEventListener("keydown", (ev) => {
  if (ev.key === "Enter" && !ev.shiftKey && (ev.ctrlKey || ev.metaKey)) {
    ev.preventDefault();
    document.getElementById("composer").dispatchEvent(new Event("submit"));
  } else if (ev.key === "Enter" && !ev.shiftKey) {
    // Plain Enter in a textarea inserts a newline; require Ctrl/Cmd+Enter
    // to send. This matches chat apps and prevents accidental sends while
    // typing multi-line code.
  }
});

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
  stopStatsPolling();
  transcriptEl.innerHTML = "";
  toolsEl.innerHTML      = '<span class="empty">No tool calls yet.</span>';
  logEl.innerHTML        = '<span class="empty">No events yet.</span>';
  inputEl.value = "";
  inputEl.style.height = "auto";
  usageHudEl.textContent = "";
  hideApproval();
  // §Phase D2.1: clear the counter badge (the JS counters live in
  // the SSE-closure scope and reset implicitly when openEventStream
  // runs again, but the DOM element persists across sessions).
  const tc = document.getElementById("tool-counter");
  if (tc) { tc.hidden = true; tc.textContent = "";
            tc.classList.remove("has-pending", "has-error"); }
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
      startStatsPolling();
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
  // §webui-approval: the backend is blocked waiting for the user to
  // approve or deny this bash command. Show the modal and let the user
  // POST a decision back to /sessions/<id>/approve.
  es.addEventListener("approval_required", (e) => {
    const d = JSON.parse(e.data);
    showApproval(d.command || "(unknown command)");
  });
  es.addEventListener("command_approval", (e) => {
    const d = JSON.parse(e.data);
    if (d.auto_approved) {
      appendLog("Info", "auto-approved: " + (d.command || ""));
    }
  });
  // §Phase D1: Tool Calls panel — each invocation gets its own card
  // keyed by OpenAI-style call_id so tool_call/progress/result events
  // for the same invocation land in one place.
  //
  // Card lifecycle:
  //   1. tool_call({id, name, args})         → create card, state=running
  //   2. tool_progress({id, progress})        → update card stage+bar
  //   3. tool_result({id, result})            → mark done/error+render
  //
  // We tolerate out-of-order progress events (only update if id is known).
  const toolCardsById = new Map();
  const toolCounter   = { pending: 0, succeeded: 0, failed: 0 };
  // §Phase D2.1: separate counter for synthesised "local-*" ids when
  // the SSE payload omits the call_id (rare; normally the sink
  // supplies OpenAI's tool_calls[].id). Kept independent from
  // toolCounter.pending so the badge stays accurate.
  let localIdFallback = 0;

  function renderToolCardHeader(card, name) {
    card.innerHTML = "";
    const head = document.createElement("div");
    head.className = "tool-card-head";
    const nameEl = document.createElement("span");
    nameEl.className = "tool-card-name";
    nameEl.textContent = name;
    const icon = document.createElement("span");
    icon.className = "tool-card-icon";
    icon.textContent = "⋯";          // placeholder, JS sets real icon
    head.appendChild(icon);
    head.appendChild(nameEl);
    const stage = document.createElement("span");
    stage.className = "tool-card-stage";
    stage.textContent = "running";
    head.appendChild(stage);
    card.appendChild(head);
    return { head, nameEl, icon, stage };
  }
  function setCardRunning(card, name) {
    card.classList.remove("success", "error");
    card.classList.add("running");
    const h = renderToolCardHeader(card, name);
    h.icon.textContent = "⟳";
    // Args (collapsed)
    const argsEl = document.createElement("div");
    argsEl.className = "tool-card-args";
    card.appendChild(argsEl);
    // Progress bar
    const wrap = document.createElement("div");
    wrap.className = "tool-card-progress";
    const bar = document.createElement("div");
    bar.className = "tool-card-progress-bar";
    wrap.appendChild(bar);
    card.appendChild(wrap);
    // Message line
    const msg = document.createElement("div");
    msg.className = "tool-card-message";
    card.appendChild(msg);
    return { argsEl, bar, msg, h };
  }
  function setCardSuccess(card) {
    card.classList.remove("running", "error", "indeterminate");
    card.classList.add("success");
    const stage = card.querySelector(".tool-card-stage");
    if (stage) stage.textContent = "done";
    const icon  = card.querySelector(".tool-card-icon");
    if (icon)  icon.textContent  = "✓";
  }
  function setCardError(card) {
    card.classList.remove("running", "indeterminate");
    card.classList.add("error");
    const stage = card.querySelector(".tool-card-stage");
    if (stage) stage.textContent = "error";
    const icon  = card.querySelector(".tool-card-icon");
    if (icon)  icon.textContent  = "✗";
  }
  function setCardArgs(card, argsJson) {
    const el = card.querySelector(".tool-card-args");
    if (!el) return;
    let pretty = "";
    try {
      const parsed = (typeof argsJson === "string")
                   ? JSON.parse(argsJson) : argsJson;
      // Skip framework-internal fields when displaying args.
      delete parsed.__daedalus_call_id;
      pretty = JSON.stringify(parsed, null, 2);
    } catch { pretty = String(argsJson); }
    el.textContent = pretty || "(no arguments)";
  }
  function setCardProgress(card, progress) {
    const bar = card.querySelector(".tool-card-progress-bar");
    const msg = card.querySelector(".tool-card-message");
    const stagePill = card.querySelector(".tool-card-stage");
    if (!bar) return;
    const stage   = progress.stage   || "executing";
    const percent = progress.percent;       // may be undefined
    const message = progress.message || "";
    if (stagePill) stagePill.textContent = stage;
    if (typeof percent === "number" && percent >= 0) {
      card.classList.remove("indeterminate");
      bar.style.width  = Math.min(100, Math.max(0, percent)) + "%";
      bar.style.transform = "";
    } else if (stage !== "done") {
      card.classList.add("indeterminate");
    }
    if (message && msg) msg.textContent = message;
  }
  function setCardResult(card, resultJson) {
    // Append a result block if not already present.
    let resEl = card.querySelector(".tool-card-result");
    if (!resEl) {
      resEl = document.createElement("div");
      resEl.className = "tool-card-result";
      card.appendChild(resEl);
    }
    resEl.textContent = (typeof resultJson === "string")
        ? resultJson
        : JSON.stringify(resultJson, null, 2);
    // §Phase D2.2: if the rendered result overflows the default
    // 120px max-height, append a "show full / collapse" toggle.
    addResultCollapseToggle(card, resEl);
  }
  // §Phase D2.2: per-card "show full / collapse" toggle for long
  // tool results. The button only appears when the rendered content
  // actually overflows the default CSS max-height (120px). When
  // toggled, the result element gets the `.expanded` class which
  // bumps max-height to 800px. The button is recreated on every
  // result update (rare — only if a tool emits multiple results
  // for the same call_id) so its handler always points at the
  // current result element.
  function addResultCollapseToggle(card, resEl) {
    // Drop any prior toggle so we don't accumulate stale buttons
    // when a result element is reused.
    const old = card.querySelector(".tool-card-collapse");
    if (old) old.remove();
    // Always reset to the default (collapsed) max-height.
    resEl.classList.remove("expanded");
    // scrollHeight reflects the full content height regardless of
    // overflow, so it tells us if the result actually overflows
    // the default 120px cap. The +4 is a tiny fudge for borders.
    const needsToggle = (resEl.scrollHeight > 124);
    if (!needsToggle) return;
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "tool-card-collapse";
    btn.textContent = "show full";
    btn.addEventListener("click", () => {
      const expanded = resEl.classList.toggle("expanded");
      btn.textContent = expanded ? "collapse" : "show full";
    });
    card.appendChild(btn);
  }
  // §Phase D2.1: re-render the "Tool calls" header badge from the
  // running toolCounter totals. Hidden when nothing has happened
  // yet; shows "N pending · N ok · N err" with only non-zero
  // buckets (so an early "1 pending" doesn't show "0 ok · 0 err").
  function renderToolCounter() {
    const el = document.getElementById("tool-counter");
    if (!el) return;
    const c = toolCounter;
    const total = c.pending + c.succeeded + c.failed;
    if (total === 0) {
      el.hidden = true;
      el.textContent = "";
      el.classList.remove("has-pending", "has-error");
      return;
    }
    el.hidden = false;
    const parts = [];
    if (c.pending   > 0) parts.push(c.pending   + " pending");
    if (c.succeeded > 0) parts.push(c.succeeded + " ok");
    if (c.failed    > 0) parts.push(c.failed    + " err");
    el.textContent = parts.join(" · ");
    el.classList.toggle("has-pending", c.pending > 0);
    el.classList.toggle("has-error",   c.failed  > 0);
  }

  es.addEventListener("tool_call", (e) => {
    if (toolsEl.querySelector(".empty")) toolsEl.innerHTML = "";
    const d = JSON.parse(e.data);
    // §Phase D2.1: use a separate counter for the rare case the SSE
    // payload omits `id` (the sink normally sets it from OpenAI's
    // tool_calls[].id). Mixing it into toolCounter.pending would
    // double-count when a fallback id is later overwritten.
    const id = d.id || ("local-" + (++localIdFallback));
    // Idempotent: if a card for this id already exists, just
    // refresh its args (e.g. on retried calls).
    let card = toolCardsById.get(id);
    if (!card) {
      card = document.createElement("div");
      card.className = "tool-card running indeterminate";
      card.dataset.id = id;
      toolsEl.appendChild(card);
      toolCardsById.set(id, card);
      // §Phase D2.1: only count once per new card.
      toolCounter.pending++;
    }
    const parts = setCardRunning(card, d.name);
    setCardArgs(card, d.args);
    renderToolCounter();
  });

  es.addEventListener("tool_progress", (e) => {
    const d = JSON.parse(e.data);
    const card = toolCardsById.get(d.id);
    if (!card) return;            // progress before call_id seen: drop.
    setCardProgress(card, d.progress);
  });

  es.addEventListener("tool_result", (e) => {
    const d = JSON.parse(e.data);
    const card = toolCardsById.get(d.id);
    if (!card) return;
    setCardResult(card, d.result);
    const status = (d.result && d.result.status) || "";
    if (status === "error" || status === "denied") {
      setCardError(card);
      toolCounter.failed++;
    } else {
      setCardSuccess(card);
      toolCounter.succeeded++;
    }
    // §Phase D2.1: every tool_result resolves one pending call.
    // (Even errors count as "done", not "still pending".)
    if (toolCounter.pending > 0) toolCounter.pending--;
    renderToolCounter();
    toolsEl.scrollTop = toolsEl.scrollHeight;
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
  inputEl.style.height = "auto";
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
  hideApproval();
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
