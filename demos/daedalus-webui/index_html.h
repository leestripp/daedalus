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

  /* === Chat message cards ========================================= */
  .msg-card {
    max-width: 88%;
    margin: 10px 0;
    padding: 10px 12px;
    border-radius: 14px;
    animation: msg-card-in 220ms ease-out;
  }
  .msg-card.user {
    margin-left: auto;
    background: #1f6feb22;
    border: 1px solid #1f6feb;
  }
  .msg-card.agent {
    margin-right: auto;
    background: #161b22;
    border: 1px solid #2a2f36;
  }
  .msg-card .meta {
    font-size: 11px; font-weight: 600;
    color: #8b949e;
    margin-bottom: 4px;
  }
  .msg-card.user .meta { color: #58a6ff; }
  .msg-card.agent .meta { color: #56d364; }
  .msg-card .content {
    white-space: pre-wrap;
    word-break: break-word;
    color: #d6d9dc;
  }
  @keyframes msg-card-in {
    from { opacity: 0; transform: translateY(6px) scale(0.98); }
    to   { opacity: 1; transform: translateY(0)   scale(1);   }
  }

  /* Collapsible thinking block inside agent cards */
  .thinking-block {
    margin: 0 0 10px;
    border: 1px dashed #2a2f36;
    border-radius: 8px;
    background: #0d1117;
    overflow: hidden;
  }
  .thinking-block summary {
    padding: 5px 9px;
    font-size: 11px;
    font-weight: 600;
    color: #8b949e;
    cursor: pointer;
    user-select: none;
    list-style: none;
  }
  .thinking-block summary::-webkit-details-marker { display: none; }
  .thinking-block summary::before {
    content: "▸ ";
    display: inline-block;
    transition: transform 160ms ease;
  }
  .thinking-block[open] > summary::before {
    transform: rotate(90deg);
  }
  .thinking-block .thinking-body {
    padding: 0 9px 8px;
    font-size: 12px;
    line-height: 1.45;
    color: #8b949e;
    white-space: pre-wrap;
    word-break: break-word;
    animation: thinking-reveal 220ms ease-out;
  }
  @keyframes thinking-reveal {
    from { opacity: 0; transform: translateY(-4px); }
    to   { opacity: 1; transform: translateY(0);   }
  }

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

  /* §webui-settings: header settings button */
  .settings-btn {
    background: transparent; color: #8b949e;
    border: 1px solid #2a2f36; padding: 4px 10px;
    font-size: 12px; border-radius: 4px;
  }
  .settings-btn:hover { background: #1f6feb22; color: #58a6ff; border-color: #1f6feb; }

  /* §webui-settings: full-page settings overlay */
  #settings-page {
    position: fixed; inset: 0; z-index: 50;
    background: #0f1115;
    display: none; flex-direction: column;
  }
  #settings-page.visible { display: flex; }
  .settings-header {
    display: flex; align-items: center; gap: 12px;
    padding: 10px 16px; border-bottom: 1px solid #2a2f36;
  }
  .settings-header h2 { margin: 0; font-size: 16px; }
  .settings-body {
    flex: 1; display: flex; overflow: hidden;
  }
  .settings-tabs {
    width: 160px; border-right: 1px solid #2a2f36;
    display: flex; flex-direction: column; padding: 8px;
    background: #161b22;
  }
  .settings-tab {
    text-align: left; background: transparent; color: #8b949e;
    border: none; padding: 8px 10px; border-radius: 4px;
    font-size: 13px; cursor: pointer;
  }
  .settings-tab.active { background: #1f6feb; color: #fff; }
  .settings-tab:hover:not(.active) { background: #2a2f36; }
  .settings-panel {
    flex: 1; padding: 16px; overflow-y: auto; display: none;
  }
  .settings-panel.active { display: block; }
  .settings-panel h3 { margin-top: 0; font-size: 14px; color: #d6d9dc; }
  .settings-form {
    max-width: 700px;
    display: grid; grid-template-columns: 140px 1fr; gap: 10px 12px;
    align-items: center; margin-bottom: 20px;
  }
  .settings-form label {
    color: #8b949e; font-size: 12px; text-align: right;
  }
  .settings-form input, .settings-form select, .settings-form textarea {
    background: #161b22; color: #d6d9dc; border: 1px solid #2a2f36;
    border-radius: 4px; padding: 6px 8px; font: inherit; font-size: 12px;
  }
  .settings-form input:focus, .settings-form select:focus, .settings-form textarea:focus {
    outline: none; border-color: #1f6feb;
  }
  .settings-form input[type=checkbox] {
    justify-self: start;
  }
  .settings-form textarea { resize: vertical; min-height: 60px; }
  .settings-form .full-width { grid-column: 1 / -1; text-align: left; }
  .settings-actions {
    grid-column: 1 / -1; display: flex; gap: 8px; justify-content: flex-end;
    margin-top: 8px;
  }
  .settings-card-list { max-width: 700px; }
  .settings-card {
    background: #161b22; border: 1px solid #2a2f36; border-radius: 6px;
    padding: 10px 12px; margin-bottom: 10px;
  }
  .settings-card h4 { margin: 0 0 6px; font-size: 13px; color: #58a6ff; }
  .settings-card p { margin: 0; font-size: 12px; color: #8b949e; }
  .settings-card .actions {
    display: flex; gap: 8px; justify-content: flex-end; margin-top: 8px;
  }
  .settings-card .actions button {
    padding: 3px 10px; font-size: 11px;
  }
  .settings-card .actions button.danger {
    background: #da3633;
  }
  .settings-note {
    font-size: 11px; color: #8b949e; margin-top: 8px;
  }
  .settings-path {
    font-family: ui-monospace, Menlo, Consolas, monospace; font-size: 11px;
    color: #6e7681;
  }
</style>
</head>
<body>
<header>
  <h1>daedalus-webui</h1>
  <span id="state">Idle</span>
  <span id="stats" class="empty">no session yet</span>
  <span id="usage-hud" title="Tokens and iterations"></span>
  <button type="button" id="settings-btn" class="settings-btn">⚙ Settings</button>
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
                placeholder="Send a message... (Enter sends, Shift+Enter newline)" disabled></textarea>
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

<!-- §webui-settings: full-page settings overlay -->
<div id="settings-page">
  <div class="settings-header">
    <h2>Settings</h2>
    <span class="settings-note">Stored in <span id="settings-db-path" class="settings-path">…</span></span>
    <div style="margin-left:auto; display:flex; gap:8px;">
      <button type="button" id="settings-reload" class="cancel">Reload</button>
      <button type="button" id="settings-close">Close</button>
    </div>
  </div>
  <div class="settings-body">
    <div class="settings-tabs">
      <button type="button" class="settings-tab active" data-tab="models">Models</button>
      <button type="button" class="settings-tab" data-tab="providers">Providers</button>
      <button type="button" class="settings-tab" data-tab="general">General</button>
      <button type="button" class="settings-tab" data-tab="mcp">MCP Servers</button>
    </div>

    <!-- Models panel -->
    <div class="settings-panel active" id="tab-models">
      <h3>Configured Models</h3>
      <div id="models-list" class="settings-card-list">
        <span class="empty">Loading…</span>
      </div>
      <h3>Discover Models</h3>
      <div id="discover-panel" class="settings-form">
        <label for="discover-source">Source model / provider</label>
        <select id="discover-source"></select>
        <div class="settings-actions">
          <button type="button" id="discover-run">Discover</button>
        </div>
        <div id="discover-results" class="settings-card-list" style="margin-top:12px;"></div>
      </div>
      <h3>Add / Edit Model</h3>
      <form id="model-form" class="settings-form">
        <input type="hidden" id="model-original-name" value="">
        <label for="model-name">Name</label>
        <input type="text" id="model-name" placeholder="minimax-m2.7" required>
        <label for="model-provider">Provider</label>
        <select id="model-provider"></select>
        <label for="model-id">Model ID</label>
        <input type="text" id="model-id" placeholder="MiniMax-M2.7">
        <label for="model-ctx">Max Context</label>
        <input type="number" id="model-ctx" placeholder="0 = auto" min="0">
        <label for="model-temp">Temperature</label>
        <input type="number" id="model-temp" step="0.1" min="0" max="2" value="0">
        <div class="settings-actions">
          <button type="button" id="model-clear" class="cancel">Clear</button>
          <button type="submit">Save Model</button>
        </div>
      </form>
    </div>

    <!-- Providers panel -->
    <div class="settings-panel" id="tab-providers">
      <h3>Configured Providers</h3>
      <div id="providers-list" class="settings-card-list">
        <span class="empty">Loading…</span>
      </div>
      <h3>Add / Edit Provider</h3>
      <form id="provider-form" class="settings-form">
        <input type="hidden" id="provider-original-name" value="">
        <label for="provider-name">Name</label>
        <input type="text" id="provider-name" placeholder="minimax" required>
        <label for="provider-host">Host URL</label>
        <input type="text" id="provider-host" placeholder="https://api.minimax.io">
        <label for="provider-key">API Key</label>
        <input type="password" id="provider-key" placeholder="sk-…">
        <label for="provider-chat-endpoint">Chat Endpoint</label>
        <input type="text" id="provider-chat-endpoint" value="/v1/chat/completions">
        <label for="provider-discovery-endpoint">Discovery Endpoint</label>
        <input type="text" id="provider-discovery-endpoint" value="/v1/models">
        <label for="provider-timeout">Timeout (seconds)</label>
        <input type="number" id="provider-timeout" min="1" value="30">
        <div class="settings-actions">
          <button type="button" id="provider-clear" class="cancel">Clear</button>
          <button type="submit">Save Provider</button>
        </div>
      </form>
    </div>

    <!-- General panel -->
    <div class="settings-panel" id="tab-general">
      <h3>General Agent Settings</h3>
      <form id="general-form" class="settings-form">
        <label for="general-default">Default Model</label>
        <select id="general-default"></select>
        <label for="general-auto">Auto-approve bash</label>
        <input type="checkbox" id="general-auto">
        <label for="general-max-iter">Max Iterations</label>
        <input type="number" id="general-max-iter" min="1">
        <label for="general-tool-size">Max Tool Result Size</label>
        <input type="number" id="general-tool-size" min="1">
        <label for="general-req-size">Max Request Size</label>
        <input type="number" id="general-req-size" min="1">
        <label for="general-res-size">Max Response Size</label>
        <input type="number" id="general-res-size" min="1">
        <label for="general-mcp-cap">MCP Schema Soft Cap %</label>
        <input type="number" id="general-mcp-cap" min="0" max="100">
        <div class="settings-actions">
          <button type="submit">Save General Settings</button>
        </div>
      </form>
      <p class="settings-note">
        Note: the webui process reads these values on session creation.
        Existing browser sessions continue using the values they started with.
      </p>
    </div>

    <!-- MCP panel -->
    <div class="settings-panel" id="tab-mcp">
      <h3>Configured MCP Servers</h3>
      <div id="mcp-list" class="settings-card-list">
        <span class="empty">Loading…</span>
      </div>
      <h3>Add / Edit MCP Server</h3>
      <form id="mcp-form" class="settings-form">
        <input type="hidden" id="mcp-original-name" value="">
        <label for="mcp-name">Name</label>
        <input type="text" id="mcp-name" placeholder="wikipedia" required>
        <label for="mcp-transport">Transport</label>
        <select id="mcp-transport">
          <option value="stdio">stdio</option>
          <option value="streamable-http">streamable-http</option>
        </select>
        <label for="mcp-command">Command</label>
        <input type="text" id="mcp-command" placeholder="mcp-server-wikipedia">
        <label for="mcp-args">Args (one per line)</label>
        <textarea id="mcp-args" placeholder="--verbose"></textarea>
        <label for="mcp-env">Env (KEY=VALUE per line)</label>
        <textarea id="mcp-env" placeholder="API_KEY=abc"></textarea>
        <label for="mcp-url">URL</label>
        <input type="text" id="mcp-url" placeholder="http://localhost:3000/sse">
        <label for="mcp-token">Bearer Token</label>
        <input type="password" id="mcp-token">
        <label for="mcp-desc">Description</label>
        <input type="text" id="mcp-desc">
        <label for="mcp-enabled">Enabled</label>
        <input type="checkbox" id="mcp-enabled" checked>
        <div class="settings-actions">
          <button type="button" id="mcp-clear" class="cancel">Clear</button>
          <button type="submit">Save Server</button>
        </div>
      </form>
    </div>
  </div>
</div>

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
let currentAgentCard = null;
let statsInterval = null;
// §webui-init: message typed before the session is ready is buffered
// and submitted as soon as POST /sessions resolves.
let pendingMessage = null;

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

// Build a user message card and add it to the transcript.
function appendUserCard(text) {
  const card = document.createElement("div");
  card.className = "msg-card user";
  const meta = document.createElement("div");
  meta.className = "meta";
  meta.textContent = "You";
  const content = document.createElement("div");
  content.className = "content";
  content.textContent = text;
  card.appendChild(meta);
  card.appendChild(content);
  transcriptEl.appendChild(card);
  transcriptEl.scrollTop = transcriptEl.scrollHeight;
}

// Create a fresh agent card for the current assistant response.
// Streaming text is accumulated into the card's raw buffer and rendered
// when the turn ends so <think>...</think> blocks can be extracted.
function ensureAgentCard() {
  if (currentAgentCard) return currentAgentCard;
  const card = document.createElement("div");
  card.className = "msg-card agent";
  const meta = document.createElement("div");
  meta.className = "meta";
  meta.textContent = "Agent";
  const body = document.createElement("div");
  body.className = "agent-body";
  const content = document.createElement("div");
  content.className = "content";
  body.appendChild(content);
  card.appendChild(meta);
  card.appendChild(body);
  transcriptEl.appendChild(card);
  transcriptEl.scrollTop = transcriptEl.scrollHeight;
  currentAgentCard = { card, body, content, raw: "" };
  return currentAgentCard;
}

// Parse <think>...</think> out of the agent's raw text and render the
// card with the thinking collapsed at the top and the answer below.
function finalizeAgentCard() {
  if (!currentAgentCard) return;
  const raw = currentAgentCard.raw;
  const card = currentAgentCard.card;
  const body = currentAgentCard.body;
  body.innerHTML = "";

  let thinking = "";
  let answer = raw;
  const thinkRegex = /<think>([\s\S]*?)<\/think>/g;
  const thoughts = [];
  let m;
  while ((m = thinkRegex.exec(raw)) !== null) {
    thoughts.push(m[1].trim());
  }
  if (thoughts.length) {
    thinking = thoughts.join("\n\n");
    answer = raw.replace(thinkRegex, "").trim();
  }

  // Strip any stray think tags that leaked out of the structured block
  // (e.g. a lone "</think>" left behind by reasoning splitting).
  answer = answer.replace(/<\/?think\s*\/?>/gi, "").trim();

  // If there's nothing meaningful to show, drop the empty card.
  if (!thinking && !answer) {
    card.remove();
    currentAgentCard = null;
    return;
  }

  if (thinking) {
    const details = document.createElement("details");
    details.className = "thinking-block";
    const summary = document.createElement("summary");
    summary.textContent = "Thinking (" + thoughts.length + ")";
    const tb = document.createElement("div");
    tb.className = "thinking-body";
    tb.textContent = thinking;
    details.appendChild(summary);
    details.appendChild(tb);
    body.appendChild(details);
  }

  const content = document.createElement("div");
  content.className = "content";
  content.textContent = answer;
  body.appendChild(content);
  transcriptEl.scrollTop = transcriptEl.scrollHeight;
  currentAgentCard = null;
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
  // §webui-init: keep input enabled while waiting for the session so
  // the user can type; only disable it while the agent is running.
  // The Send button is disabled while a message is already buffered
  // for the upcoming session.
  sendBtn.disabled   = running || (pendingMessage !== null);
  cancelBtn.disabled = !sessionId || !running;
  inputEl.disabled   = running;
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
  // §webui-init: plain Enter sends; Shift+Enter inserts a newline for
  // multi-line input. This matches the behaviour of most chat UIs and
  // fixes the "nothing happens" symptom when the user hits Enter.
  if (ev.key === "Enter" && !ev.shiftKey) {
    ev.preventDefault();
    document.getElementById("composer").dispatchEvent(new Event("submit"));
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
  currentDelta = null;
  currentAgentCard = null;
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
      // §webui-init: enable controls through setState() so the pending-
      // message gate is evaluated correctly.
      setState(stateEl.dataset.lastName || "Idle");
      inputEl.focus();

      // §webui-init: flush any message the user sent while the session
      // was still being created.
      if (pendingMessage) {
        const text = pendingMessage;
        pendingMessage = null;
        hideThinkingBubble();
        setStage("Idle");
        submitMessageToServer(text);
        setState(stateEl.dataset.lastName || "Idle");
      }
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
    const agent = ensureAgentCard();
    agent.raw += d.text || "";
    agent.content.textContent = agent.raw;
    transcriptEl.scrollTop = transcriptEl.scrollHeight;
  });
  es.addEventListener("assistant_end", () => {
    finalizeAgentCard();
    currentDelta = null;
  });
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

// §webui-init: shared network send helper. Parses JSON only when the
// server actually returns a JSON body, so a future 204 refactor won't
// throw on the client.
function submitMessageToServer(text) {
  fetch("/sessions/" + sessionId + "/messages", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ message: text }),
  })
    .then(async r => {
      const ct = r.headers.get("content-type") || "";
      const j = ct.includes("application/json") ? await r.json() : {};
      if (j.error) appendLog("Error", j.error);
    })
    .catch(e => appendLog("Error", "send failed: " + e));
}

document.getElementById("composer").addEventListener("submit", (ev) => {
  ev.preventDefault();
  const text = inputEl.value.trim();
  if (!text) return;
  inputEl.value = "";
  inputEl.style.height = "auto";
  appendUserCard(text);

  // §webui-init: if the session is still being created, buffer this
  // message and submit it as soon as POST /sessions resolves. The UI
  // already shows the message, so it doesn't feel like a no-op.
  if (!sessionId) {
    pendingMessage = text;
    setState(stateEl.dataset.lastName);
    return;
  }

  // §Phase 9.10: clean up any leftover indicator from a previous turn
  // (the chip row was hidden on the previous terminal state, but be
  // defensive). The next state_change event will re-show them.
  hideThinkingBubble();
  setStage("Idle");
  submitMessageToServer(text);
});

cancelBtn.addEventListener("click", () => {
  if (!sessionId) return;
  hideApproval();
  fetch("/sessions/" + sessionId + "/cancel", { method: "POST" })
    .catch(e => appendLog("Error", "cancel failed: " + e));
});

// ============================================================================
// §webui-settings: settings page logic
// ============================================================================
const settingsPage      = $("settings-page");
const settingsBtn       = $("settings-btn");
const settingsCloseBtn  = $("settings-close");
const settingsReloadBtn = $("settings-reload");
const settingsDbPathEl  = $("settings-db-path");
const settingsTabs      = document.querySelectorAll(".settings-tab");
const settingsPanels    = document.querySelectorAll(".settings-panel");

let cachedModels = [];
let cachedProviders = [];
let cachedGeneral = {};

settingsBtn.addEventListener("click", () => {
  settingsPage.classList.add("visible");
  loadSettings();
});
settingsCloseBtn.addEventListener("click", () => settingsPage.classList.remove("visible"));
settingsReloadBtn.addEventListener("click", loadSettings);

settingsTabs.forEach(tab => {
  tab.addEventListener("click", () => {
    settingsTabs.forEach(t => t.classList.remove("active"));
    settingsPanels.forEach(p => p.classList.remove("active"));
    tab.classList.add("active");
    document.getElementById("tab-" + tab.dataset.tab).classList.add("active");
  });
});

function showToast(msg) {
  appendLog("Info", msg);
}

async function loadSettings() {
  try {
    const [modelsResp, providersResp, generalResp, mcpResp] = await Promise.all([
      fetch("/settings/models"),
      fetch("/settings/providers"),
      fetch("/settings/general"),
      fetch("/settings/mcp"),
    ]);
    const modelsData = await modelsResp.json();
    const providersData = await providersResp.json();
    const generalData = await generalResp.json();
    const mcpData = await mcpResp.json();

    cachedModels = (modelsData.models || []).sort((a, b) => a.name.localeCompare(b.name));
    cachedProviders = (providersData.providers || []).sort((a, b) => a.name.localeCompare(b.name));
    cachedGeneral = generalData;
    settingsDbPathEl.textContent = generalData.db_path || "(unknown)";

    renderModelsList();
    populateModelProviderDropdown();
    populateDiscoverSourceDropdown();
    populateDefaultModelDropdown();
    renderProvidersList();
    renderGeneralForm();
    renderMcpList(mcpData.servers || []);
  } catch (e) {
    appendLog("Error", "failed to load settings: " + e);
  }
}

// ---- Models ----
function renderModelsList() {
  const list = $("models-list");
  if (cachedModels.length === 0) {
    list.innerHTML = '<span class="empty">No models configured.</span>';
    return;
  }
  list.innerHTML = "";
  cachedModels.forEach(m => {
    const card = document.createElement("div");
    card.className = "settings-card";
    const title = document.createElement("h4");
    title.textContent = m.name + (cachedGeneral.default_model === m.name ? " (default)" : "");
    card.appendChild(title);
    const meta = document.createElement("p");
    meta.textContent = (m.model_id || "(no model id)") +
                       " @ " + (m.provider || "(no provider)") +
                       " | ctx=" + (m.max_context_size || "auto") +
                       " | temp=" + m.temperature;
    card.appendChild(meta);
    const actions = document.createElement("div");
    actions.className = "actions";
    const editBtn = document.createElement("button");
    editBtn.type = "button";
    editBtn.textContent = "Edit";
    editBtn.addEventListener("click", () => populateModelForm(m));
    const defaultBtn = document.createElement("button");
    defaultBtn.type = "button";
    defaultBtn.textContent = "Set default";
    defaultBtn.addEventListener("click", () => setDefaultModel(m.name));
    const delBtn = document.createElement("button");
    delBtn.type = "button";
    delBtn.className = "danger";
    delBtn.textContent = "Delete";
    delBtn.addEventListener("click", () => deleteModel(m.name));
    actions.appendChild(editBtn);
    actions.appendChild(defaultBtn);
    actions.appendChild(delBtn);
    card.appendChild(actions);
    list.appendChild(card);
  });
}

function populateModelProviderDropdown() {
  const sel = $("model-provider");
  sel.innerHTML = '<option value="">(none)</option>';
  cachedProviders.forEach(p => {
    const opt = document.createElement("option");
    opt.value = p.name;
    opt.textContent = p.name;
    sel.appendChild(opt);
  });
}

function populateModelForm(m) {
  $("model-original-name").value = m.name || "";
  $("model-name").value = m.name || "";
  $("model-provider").value = m.provider || "";
  $("model-id").value = m.model_id || "";
  $("model-ctx").value = m.max_context_size || "";
  $("model-temp").value = m.temperature;
}

$("model-clear").addEventListener("click", () => {
  $("model-form").reset();
  $("model-original-name").value = "";
  $("model-temp").value = 0;
});

$("model-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const original = $("model-original-name").value;
  const name = $("model-name").value.trim();
  if (!name) return;
  const payload = {
    name: name,
    provider: $("model-provider").value.trim(),
    model_id: $("model-id").value.trim(),
    max_context_size: parseInt($("model-ctx").value, 10) || 0,
    temperature: parseFloat($("model-temp").value) || 0,
  };
  try {
    const r = await fetch("/settings/models", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    });
    const j = await r.json();
    if (j.error) throw new Error(j.error);
    showToast("Model saved: " + name);
    $("model-form").reset();
    $("model-original-name").value = "";
    $("model-temp").value = 0;
    await loadSettings();
  } catch (e) {
    appendLog("Error", "save model failed: " + e);
  }
});

async function setDefaultModel(name) {
  try {
    const r = await fetch("/settings/models/default", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ name }),
    });
    const j = await r.json();
    if (j.error) throw new Error(j.error);
    showToast("Default model set: " + name);
    await loadSettings();
  } catch (e) {
    appendLog("Error", "set default model failed: " + e);
  }
}

async function deleteModel(name) {
  if (!confirm("Delete model '" + name + "'?")) return;
  try {
    const r = await fetch("/settings/models/" + encodeURIComponent(name), { method: "DELETE" });
    const j = await r.json();
    if (j.error) throw new Error(j.error);
    showToast("Model deleted: " + name);
    await loadSettings();
  } catch (e) {
    appendLog("Error", "delete model failed: " + e);
  }
}

// ---- Model discovery ----
function populateDiscoverSourceDropdown() {
  const sel = $("discover-source");
  sel.innerHTML = '<option value="">(select source)</option>';
  // Prefer configured models: discover uses the model's provider.
  cachedModels.forEach(m => {
    const opt = document.createElement("option");
    opt.value = m.name;
    opt.textContent = "model: " + m.name;
    sel.appendChild(opt);
  });
  // Also allow discovering directly from a provider.
  cachedProviders.forEach(p => {
    const opt = document.createElement("option");
    opt.value = "provider:" + p.name;
    opt.textContent = "provider: " + p.name;
    sel.appendChild(opt);
  });
}

$("discover-run").addEventListener("click", async () => {
  const sel = $("discover-source");
  const source = sel.value;
  if (!source) return;
  const isProvider = source.startsWith("provider:");
  const name = isProvider ? source.slice("provider:".length) : source;
  const endpoint = isProvider
      ? "/settings/providers/discover?name=" + encodeURIComponent(name)
      : "/settings/models/discover?name=" + encodeURIComponent(name);
  const providerName = isProvider ? name : "";
  const resultsEl = $("discover-results");
  resultsEl.innerHTML = '<span class="empty">Discovering…</span>';
  try {
    const r = await fetch(endpoint);
    const j = await r.json();
    if (j.error) throw new Error(j.error);
    const models = j.models || [];
    if (models.length === 0) {
      resultsEl.innerHTML = '<span class="empty">No models discovered.</span>';
      return;
    }
    resultsEl.innerHTML = "";
    models.forEach(m => {
      const card = document.createElement("div");
      card.className = "settings-card";
      const title = document.createElement("h4");
      title.textContent = m.id;
      card.appendChild(title);
      const meta = document.createElement("p");
      const badges = [];
      if (m.owned_by) badges.push("owned by " + m.owned_by);
      if (m.context_length) badges.push("ctx=" + m.context_length);
      if (m.supports_vision) badges.push("vision");
      if (m.supports_reasoning) badges.push("reasoning");
      meta.textContent = badges.join(" | ") || "(no metadata)";
      card.appendChild(meta);
      const actions = document.createElement("div");
      actions.className = "actions";
      const addBtn = document.createElement("button");
      addBtn.type = "button";
      addBtn.textContent = "Add as Model";
      addBtn.addEventListener("click", () => {
        $("model-name").value = m.id;
        $("model-provider").value = providerName;
        $("model-id").value = m.id;
        $("model-ctx").value = m.context_length || "";
        $("model-temp").value = 0;
        document.querySelector('.settings-tab[data-tab="models"]').click();
      });
      actions.appendChild(addBtn);
      card.appendChild(actions);
      resultsEl.appendChild(card);
    });
  } catch (e) {
    resultsEl.innerHTML = '<span class="empty">Discovery failed: ' + e + '</span>';
    appendLog("Error", "model discovery failed: " + e);
  }
});

// ---- Providers ----
function renderProvidersList() {
  const list = $("providers-list");
  if (cachedProviders.length === 0) {
    list.innerHTML = '<span class="empty">No providers configured.</span>';
    return;
  }
  list.innerHTML = "";
  cachedProviders.forEach(p => {
    const card = document.createElement("div");
    card.className = "settings-card";
    const title = document.createElement("h4");
    title.textContent = p.name;
    card.appendChild(title);
    const meta = document.createElement("p");
    meta.textContent = p.host +
                       " | chat=" + p.chat_endpoint +
                       " | discover=" + p.discovery_endpoint +
                       " | timeout=" + p.timeout_seconds + "s";
    card.appendChild(meta);
    const actions = document.createElement("div");
    actions.className = "actions";
    const editBtn = document.createElement("button");
    editBtn.type = "button";
    editBtn.textContent = "Edit";
    editBtn.addEventListener("click", () => populateProviderForm(p));
    const delBtn = document.createElement("button");
    delBtn.type = "button";
    delBtn.className = "danger";
    delBtn.textContent = "Delete";
    delBtn.addEventListener("click", () => deleteProvider(p.name));
    actions.appendChild(editBtn);
    actions.appendChild(delBtn);
    card.appendChild(actions);
    list.appendChild(card);
  });
}

function populateProviderForm(p) {
  $("provider-original-name").value = p.name || "";
  $("provider-name").value = p.name || "";
  $("provider-host").value = p.host || "";
  // API key is masked on GET; leave blank to preserve existing secret.
  $("provider-key").value = "";
  $("provider-chat-endpoint").value = p.chat_endpoint || "/v1/chat/completions";
  $("provider-discovery-endpoint").value = p.discovery_endpoint || "/v1/models";
  $("provider-timeout").value = p.timeout_seconds || 30;
}

$("provider-clear").addEventListener("click", () => {
  $("provider-form").reset();
  $("provider-original-name").value = "";
  $("provider-chat-endpoint").value = "/v1/chat/completions";
  $("provider-discovery-endpoint").value = "/v1/models";
  $("provider-timeout").value = 30;
});

$("provider-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const name = $("provider-name").value.trim();
  if (!name) return;
  const payload = {
    name: name,
    host: $("provider-host").value.trim(),
    api_key: $("provider-key").value,
    chat_endpoint: $("provider-chat-endpoint").value.trim() || "/v1/chat/completions",
    discovery_endpoint: $("provider-discovery-endpoint").value.trim() || "/v1/models",
    timeout_seconds: parseInt($("provider-timeout").value, 10) || 30,
  };
  try {
    const r = await fetch("/settings/providers", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    });
    const j = await r.json();
    if (j.error) throw new Error(j.error);
    showToast("Provider saved: " + name);
    $("provider-form").reset();
    $("provider-original-name").value = "";
    $("provider-chat-endpoint").value = "/v1/chat/completions";
    $("provider-discovery-endpoint").value = "/v1/models";
    $("provider-timeout").value = 30;
    await loadSettings();
  } catch (e) {
    appendLog("Error", "save provider failed: " + e);
  }
});

async function deleteProvider(name) {
  if (!confirm("Delete provider '" + name + "'? Models referencing it will no longer resolve.")) return;
  try {
    const r = await fetch("/settings/providers/" + encodeURIComponent(name), { method: "DELETE" });
    const j = await r.json();
    if (j.error) throw new Error(j.error);
    showToast("Provider deleted: " + name);
    await loadSettings();
  } catch (e) {
    appendLog("Error", "delete provider failed: " + e);
  }
}

function populateDefaultModelDropdown() {
  const sel = $("general-default");
  sel.innerHTML = '<option value="">(none)</option>';
  cachedModels.forEach(m => {
    const opt = document.createElement("option");
    opt.value = m.name;
    opt.textContent = m.name;
    if (cachedGeneral.default_model === m.name) opt.selected = true;
    sel.appendChild(opt);
  });
}

// ---- General ----
function renderGeneralForm() {
  $("general-auto").checked = !!cachedGeneral.auto_approve;
  $("general-max-iter").value = cachedGeneral.max_agent_iterations ?? 100;
  $("general-tool-size").value = cachedGeneral.max_tool_result_size ?? 10000;
  $("general-req-size").value = cachedGeneral.max_request_size ?? 4000000;
  $("general-res-size").value = cachedGeneral.max_response_size ?? 5000000;
  $("general-mcp-cap").value = cachedGeneral.mcp_tool_schema_soft_cap_percent ?? 5;
}

$("general-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const payload = {
    default_model: $("general-default").value,
    auto_approve: $("general-auto").checked,
    max_agent_iterations: parseInt($("general-max-iter").value, 10),
    max_tool_result_size: parseInt($("general-tool-size").value, 10),
    max_request_size: parseInt($("general-req-size").value, 10),
    max_response_size: parseInt($("general-res-size").value, 10),
    mcp_tool_schema_soft_cap_percent: parseInt($("general-mcp-cap").value, 10),
  };
  try {
    const r = await fetch("/settings/general", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    });
    const j = await r.json();
    if (j.error) throw new Error(j.error);
    showToast("General settings saved");
    await loadSettings();
  } catch (e) {
    appendLog("Error", "save general settings failed: " + e);
  }
});

// ---- MCP ----
function renderMcpList(servers) {
  const list = $("mcp-list");
  if (servers.length === 0) {
    list.innerHTML = '<span class="empty">No MCP servers configured.</span>';
    return;
  }
  list.innerHTML = "";
  servers.sort((a, b) => a.name.localeCompare(b.name));
  servers.forEach(s => {
    const card = document.createElement("div");
    card.className = "settings-card";
    const title = document.createElement("h4");
    title.textContent = s.name + (s.enabled ? "" : " (disabled)");
    card.appendChild(title);
    const meta = document.createElement("p");
    meta.textContent = s.transport + " | " +
                       (s.transport === "stdio" ? (s.command || "(no command)") : (s.url || "(no url)"));
    card.appendChild(meta);
    const actions = document.createElement("div");
    actions.className = "actions";
    const editBtn = document.createElement("button");
    editBtn.type = "button";
    editBtn.textContent = "Edit";
    editBtn.addEventListener("click", () => populateMcpForm(s));
    const delBtn = document.createElement("button");
    delBtn.type = "button";
    delBtn.className = "danger";
    delBtn.textContent = "Delete";
    delBtn.addEventListener("click", () => deleteMcpServer(s.name));
    actions.appendChild(editBtn);
    actions.appendChild(delBtn);
    card.appendChild(actions);
    list.appendChild(card);
  });
}

function populateMcpForm(s) {
  $("mcp-original-name").value = s.name || "";
  $("mcp-name").value = s.name || "";
  $("mcp-transport").value = s.transport || "stdio";
  $("mcp-command").value = s.command || "";
  $("mcp-args").value = (s.args || []).join("\n");
  $("mcp-env").value = Object.entries(s.env || {}).map(([k, v]) => k + "=" + v).join("\n");
  $("mcp-url").value = s.url || "";
  // The bearer token returned by GET is masked; leave the field blank so
  // the user only has to type a new token when they actually want to change it.
  $("mcp-token").value = "";
  $("mcp-desc").value = s.description || "";
  $("mcp-enabled").checked = s.enabled !== false;
}

$("mcp-clear").addEventListener("click", () => {
  $("mcp-form").reset();
  $("mcp-original-name").value = "";
  $("mcp-transport").value = "stdio";
  $("mcp-enabled").checked = true;
});

$("mcp-form").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const name = $("mcp-name").value.trim();
  if (!name) return;
  const env = {};
  $("mcp-env").value.split("\n").forEach(line => {
    const eq = line.indexOf("=");
    if (eq > 0) env[line.slice(0, eq).trim()] = line.slice(eq + 1).trim();
  });
  const payload = {
    name: name,
    transport: $("mcp-transport").value,
    command: $("mcp-command").value.trim(),
    args: $("mcp-args").value.split("\n").map(s => s.trim()).filter(Boolean),
    env: env,
    url: $("mcp-url").value.trim(),
    bearer_token: $("mcp-token").value,
    description: $("mcp-desc").value.trim(),
    enabled: $("mcp-enabled").checked,
  };
  try {
    const r = await fetch("/settings/mcp", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    });
    const j = await r.json();
    if (j.error) throw new Error(j.error);
    showToast("MCP server saved: " + name);
    $("mcp-form").reset();
    $("mcp-original-name").value = "";
    $("mcp-transport").value = "stdio";
    $("mcp-enabled").checked = true;
    await loadSettings();
  } catch (e) {
    appendLog("Error", "save MCP server failed: " + e);
  }
});

async function deleteMcpServer(name) {
  if (!confirm("Delete MCP server '" + name + "'?")) return;
  try {
    const r = await fetch("/settings/mcp/" + encodeURIComponent(name), { method: "DELETE" });
    const j = await r.json();
    if (j.error) throw new Error(j.error);
    showToast("MCP server deleted: " + name);
    await loadSettings();
  } catch (e) {
    appendLog("Error", "delete MCP server failed: " + e);
  }
}

// §webui-init: start creating the session BEFORE enabling the input so
// any transcript-clear in newSession() happens while the user still can't
// type. setState("Idle") enables the composer once the UI is ready.
newSession();
setState("Idle");

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
