"use strict";

const crypto = require("crypto");
const fs = require("fs");
const os = require("os");
const path = require("path");
const vscode = require("vscode");
const {
  AcpClient,
  textBlock,
  resourceLinkBlock,
  embeddedResourceBlock,
} = require("./acp_client");

const BEFORE_SCHEME = "deskcode-before";
const VIEW_ID = "deskcode.chatView";

const MODE_DEFINITIONS = [
  {
    id: "ask",
    label: "Ask",
    detail: "Confirm every edit, shell command, and external action.",
  },
  {
    id: "edit",
    label: "Edit",
    detail: "Auto-approve workspace file edits and read-only commands; ask before other commands.",
  },
  {
    id: "full",
    label: "Full",
    detail: "Run all enabled workspace tools without asking.",
  },
  {
    id: "plan",
    label: "Plan",
    detail: "Investigate with read-only tools and propose a plan without modifying files.",
  },
];

class BeforeDocumentContentProvider {
  constructor() {
    this._emitter = new vscode.EventEmitter();
    this.onDidChange = this._emitter.event;
    this._snapshots = new Map();
  }

  store(uriKey, content) {
    if (this._snapshots.size >= 250 && !this._snapshots.has(uriKey)) {
      const oldestKey = this._snapshots.keys().next().value;
      this._snapshots.delete(oldestKey);
    }
    this._snapshots.set(uriKey, typeof content === "string" ? content : "");
    this._emitter.fire(vscode.Uri.parse(`${BEFORE_SCHEME}:${uriKey}`));
  }

  provideTextDocumentContent(uri) {
    const key = uri.path || "";
    if (this._snapshots.has(key)) {
      return this._snapshots.get(key);
    }
    const decoded = decodeURIComponent(key);
    return this._snapshots.get(decoded) || "";
  }

  dispose() {
    this._snapshots.clear();
    this._emitter.dispose();
  }
}

function getWorkspaceRoot() {
  const folders = vscode.workspace.workspaceFolders;
  if (folders && folders.length > 0 && folders[0].uri && folders[0].uri.fsPath) {
    return folders[0].uri.fsPath;
  }
  const activeEditor = vscode.window.activeTextEditor;
  if (activeEditor && activeEditor.document && activeEditor.document.uri.scheme === "file") {
    return path.dirname(activeEditor.document.uri.fsPath);
  }
  return os.homedir();
}

function getDeskConfig() {
  const cfg = vscode.workspace.getConfiguration("deskcode");
  const executablePath = String(cfg.get("executablePath", "desk") || "desk").trim() || "desk";
  const rawMode = String(cfg.get("defaultMode", "edit") || "edit");
  const defaultMode = ["ask", "edit", "full", "plan"].includes(rawMode) ? rawMode : "edit";
  const rawExtra = cfg.get("extraArgs", []);
  const extraArgs = Array.isArray(rawExtra) ? rawExtra.map(String) : [];
  return { executablePath, defaultMode, extraArgs };
}

class DeskCodeChatViewProvider {
  constructor(context, beforeProvider) {
    this._context = context;
    this._beforeProvider = beforeProvider;
    this._view = null;
    this._client = null;
    this._sessionId = null;
    this._currentMode = getDeskConfig().defaultMode;
    this._turnRunning = false;
    this._diffSeq = 0;
    this._permSeq = 0;
    this._diffs = new Map();
    this._pendingPermissions = new Map();
    this._pendingMessages = [];
    this._ready = false;
  }

  resolveWebviewView(webviewView) {
    this._view = webviewView;
    this._ready = false;

    webviewView.webview.options = {
      enableScripts: true,
      localResourceRoots: [
        vscode.Uri.file(path.join(this._context.extensionPath, "media")),
      ],
    };

    const nonce = crypto.randomBytes(16).toString("base64");
    webviewView.webview.html = this._buildHtml(webviewView.webview, nonce);

    webviewView.webview.onDidReceiveMessage((msg) => {
      this._onWebviewMessage(msg);
    });

    webviewView.onDidDispose(() => {
      if (this._view === webviewView) {
        this._view = null;
        this._ready = false;
      }
    });
  }

  async reveal() {
    if (this._view && typeof this._view.show === "function") {
      this._view.show(true);
      return;
    }
    try {
      await vscode.commands.executeCommand(`${VIEW_ID}.focus`);
    } catch (_err) {
      // View container may not be mounted in headless tests.
    }
  }

  _post(message) {
    if (this._view && this._ready) {
      this._view.webview.postMessage(message);
    } else {
      this._pendingMessages.push(message);
    }
  }

  _flushPendingMessages() {
    if (!this._view || !this._ready) {
      return;
    }
    while (this._pendingMessages.length > 0) {
      this._view.webview.postMessage(this._pendingMessages.shift());
    }
  }

  async _onWebviewMessage(msg) {
    if (!msg || typeof msg !== "object") {
      return;
    }
    switch (msg.type) {
      case "ready":
        this._ready = true;
        this._post({
          type: "state",
          mode: this._currentMode,
          running: this._turnRunning,
          sessionId: this._sessionId,
        });
        this._flushPendingMessages();
        break;
      case "prompt":
        if (typeof msg.text === "string" && msg.text.trim()) {
          await this.sendUserPrompt(msg.text.trim());
        }
        break;
      case "cancel":
        this.cancelCurrentTurn();
        break;
      case "newSession":
        await this.startNewSession();
        break;
      case "setMode":
        if (typeof msg.mode === "string") {
          await this.switchMode(msg.mode);
        }
        break;
      case "permissionReply":
        this._resolvePermission(String(msg.requestId || ""), msg.optionId || null);
        break;
      case "viewDiff":
        await this._openDiff(String(msg.diffId || ""));
        break;
      case "openLocation":
        await this._openLocation(String(msg.path || ""), msg.line);
        break;
      default:
        break;
    }
  }

  _cancelAllPendingPermissions() {
    for (const [reqId, resolver] of this._pendingPermissions.entries()) {
      resolver(null);
      this._post({
        type: "permissionResolved",
        requestId: reqId,
        outcome: "cancelled",
      });
    }
    this._pendingPermissions.clear();
  }

  _resolvePermission(requestId, optionId) {
    const resolver = this._pendingPermissions.get(requestId);
    if (!resolver) {
      return;
    }
    this._pendingPermissions.delete(requestId);
    resolver(typeof optionId === "string" && optionId ? optionId : null);
    this._post({
      type: "permissionResolved",
      requestId,
      outcome: optionId || "cancelled",
    });
  }

  async _ensureSession() {
    const cfg = getDeskConfig();
    const cwd = getWorkspaceRoot();

    if (!this._client || !this._client.isRunning) {
      this._sessionId = null;
      this._client = new AcpClient({
        executablePath: cfg.executablePath,
        extraArgs: cfg.extraArgs,
        cwd,
      });
      this._client.on("exit", (err) => {
        this._sessionId = null;
        this._turnRunning = false;
        this._cancelAllPendingPermissions();
        this._post({
          type: "state",
          mode: this._currentMode,
          running: false,
          sessionId: null,
        });
        if (err && err.exitCode !== 0 && err.exitCode !== null) {
          this._post({
            type: "error",
            message: err.message,
          });
        }
      });
      await this._client.start();
    }

    if (!this._sessionId) {
      const mode = this._currentMode || cfg.defaultMode;
      const session = await this._client.newSession({ cwd, mode });
      this._sessionId = session.sessionId;
      this._currentMode = session.currentModeId || mode;
      this._post({
        type: "state",
        mode: this._currentMode,
        running: this._turnRunning,
        sessionId: this._sessionId,
      });
    }

    return { client: this._client, sessionId: this._sessionId };
  }

  async startNewSession() {
    await this.reveal();
    this.cancelCurrentTurn();
    this._diffs.clear();
    this._sessionId = null;
    this._currentMode = getDeskConfig().defaultMode;
    this._post({
      type: "reset",
      mode: this._currentMode,
    });
    try {
      await this._ensureSession();
    } catch (err) {
      this._post({
        type: "error",
        message: err && err.message ? err.message : String(err),
      });
    }
  }

  cancelCurrentTurn() {
    this._cancelAllPendingPermissions();
    if (this._client && this._client.isRunning && this._sessionId && this._turnRunning) {
      try {
        this._client.cancel(this._sessionId);
      } catch (_err) {
        // Ignore if process exited concurrently.
      }
    }
  }

  async switchMode(modeId) {
    if (!["ask", "edit", "full", "plan"].includes(modeId)) {
      return;
    }
    this._currentMode = modeId;
    this._post({
      type: "modeChanged",
      mode: modeId,
    });
    if (this._client && this._client.isRunning && this._sessionId) {
      try {
        await this._client.setMode(this._sessionId, modeId);
      } catch (err) {
        this._post({
          type: "error",
          message: err && err.message ? err.message : String(err),
        });
      }
    }
  }

  async sendUserPrompt(displayText, extraBlocks = [], contextLabel = "") {
    await this.reveal();
    if (this._turnRunning) {
      vscode.window.showWarningMessage("DeskCode is still running the current turn. Stop it first or wait for completion.");
      return null;
    }

    const blocks = [];
    if (displayText) {
      blocks.push(textBlock(displayText));
    }
    for (const block of extraBlocks) {
      blocks.push(block);
    }
    if (blocks.length === 0) {
      return null;
    }

    this._post({
      type: "userMessage",
      text: displayText,
      contextLabel,
    });

    this._turnRunning = true;
    this._post({
      type: "turnStarted",
      mode: this._currentMode,
    });

    try {
      const { client, sessionId } = await this._ensureSession();
      const result = await client.prompt(sessionId, blocks, {
        onUpdate: (update) => this._handleSessionUpdate(update),
        onPermission: (req) => this._requestWebviewPermission(req),
      });
      this._turnRunning = false;
      this._post({
        type: "turnFinished",
        stopReason: result.stopReason,
      });
      return result;
    } catch (err) {
      this._turnRunning = false;
      this._cancelAllPendingPermissions();
      const message = err && err.message ? err.message : String(err);
      this._post({
        type: "error",
        message,
      });
      this._post({
        type: "turnFinished",
        stopReason: "error",
      });
      return null;
    }
  }

  _handleSessionUpdate(update) {
    if (update.kind === "agent_message_chunk") {
      this._post({
        type: "assistantChunk",
        text: update.text,
      });
      return;
    }

    if (update.kind === "tool_call") {
      const diffEntries = this._registerDiffs(update.diffs || []);
      this._post({
        type: "toolCall",
        toolCallId: update.toolCallId,
        title: update.title,
        toolKind: update.toolKind,
        status: update.status,
        locations: update.locations || [],
        text: update.text || "",
        diffs: diffEntries,
      });
      return;
    }

    if (update.kind === "tool_call_update") {
      const diffEntries = this._registerDiffs(update.diffs || []);
      this._post({
        type: "toolCallUpdate",
        toolCallId: update.toolCallId,
        title: update.title,
        status: update.status,
        locations: update.locations || [],
        text: update.text || "",
        diffs: diffEntries,
      });
      return;
    }

    if (update.kind === "plan") {
      this._post({
        type: "plan",
        entries: update.entries || [],
      });
      return;
    }

    if (update.kind === "current_mode_update" && update.currentModeId) {
      this._currentMode = update.currentModeId;
      this._post({
        type: "modeChanged",
        mode: update.currentModeId,
      });
    }
  }

  _registerDiffs(diffs) {
    const out = [];
    for (const diff of diffs) {
      if (!diff || !diff.path) {
        continue;
      }
      const diffId = `diff-${++this._diffSeq}`;
      const baseName = path.basename(diff.path);
      const beforePath = `/before/${diffId}/${encodeURIComponent(baseName)}`;
      const afterPath = `/after/${diffId}/${encodeURIComponent(baseName)}`;
      this._beforeProvider.store(beforePath, diff.oldText === null ? "" : diff.oldText);
      this._beforeProvider.store(afterPath, diff.newText || "");
      const record = {
        diffId,
        path: diff.path,
        fileName: baseName,
        beforePath,
        afterPath,
        isNewFile: diff.oldText === null,
      };
      this._diffs.set(diffId, record);
      out.push({
        diffId,
        path: diff.path,
        fileName: baseName,
        isNewFile: diff.oldText === null,
      });
    }
    return out;
  }

  async _openDiff(diffId) {
    const record = this._diffs.get(diffId);
    if (!record) {
      return;
    }
    const beforeUri = vscode.Uri.parse(`${BEFORE_SCHEME}:${record.beforePath}`);
    const afterUri = fs.existsSync(record.path)
      ? vscode.Uri.file(record.path)
      : vscode.Uri.parse(`${BEFORE_SCHEME}:${record.afterPath}`);
    const title = record.isNewFile
      ? `${record.fileName} (New File — DeskCode)`
      : `${record.fileName} (Before ↔ After — DeskCode)`;
    await vscode.commands.executeCommand("vscode.diff", beforeUri, afterUri, title);
  }

  async _openLocation(filePath, line) {
    if (!filePath || !fs.existsSync(filePath)) {
      return;
    }
    const doc = await vscode.workspace.openTextDocument(vscode.Uri.file(filePath));
    const lineIdx = Number.isInteger(line) && line > 0 ? line - 1 : 0;
    const pos = new vscode.Position(lineIdx, 0);
    await vscode.window.showTextDocument(doc, {
      selection: new vscode.Range(pos, pos),
      preview: true,
    });
  }

  _requestWebviewPermission(req) {
    const requestId = `perm-${++this._permSeq}`;
    const rawInput = req.toolCall && req.toolCall.rawInput ? req.toolCall.rawInput : {};
    const detail =
      rawInput.command ||
      rawInput.path ||
      rawInput.description ||
      "";
    return new Promise((resolve) => {
      this._pendingPermissions.set(requestId, resolve);
      this._post({
        type: "permissionRequest",
        requestId,
        title: (req.toolCall && req.toolCall.title) || "Approval required",
        toolKind: (req.toolCall && req.toolCall.toolKind) || "other",
        detail: String(detail),
        options: req.options || [],
      });
    });
  }

  async restartClient() {
    this._cancelAllPendingPermissions();
    this._sessionId = null;
    if (this._client) {
      const old = this._client;
      this._client = null;
      await old.close();
    }
  }

  async dispose() {
    await this.restartClient();
  }

  _buildHtml(webview, nonce) {
    const csp = [
      "default-src 'none'",
      `style-src ${webview.cspSource} 'nonce-${nonce}'`,
      `script-src 'nonce-${nonce}'`,
    ].join("; ");

    return `<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta http-equiv="Content-Security-Policy" content="${csp}">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>DeskCode</title>
  <style nonce="${nonce}">
    :root {
      color-scheme: light dark;
    }
    * {
      box-sizing: border-box;
    }
    body {
      margin: 0;
      padding: 0;
      height: 100vh;
      display: flex;
      flex-direction: column;
      font-family: var(--vscode-font-family);
      font-size: var(--vscode-font-size);
      color: var(--vscode-foreground);
      background: var(--vscode-sideBar-background, var(--vscode-editor-background));
    }
    .toolbar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 6px;
      padding: 8px 10px;
      border-bottom: 1px solid var(--vscode-panel-border, var(--vscode-widget-border, rgba(128,128,128,0.25)));
    }
    .toolbar-left {
      display: flex;
      align-items: center;
      gap: 6px;
      min-width: 0;
    }
    .status-dot {
      width: 8px;
      height: 8px;
      border-radius: 50%;
      background: var(--vscode-descriptionForeground);
      flex-shrink: 0;
    }
    .status-dot.connected {
      background: var(--vscode-testing-iconPassed, #3fb950);
    }
    .status-dot.running {
      background: var(--vscode-progressBar-background, #0078d4);
    }
    select.mode-select {
      background: var(--vscode-dropdown-background);
      color: var(--vscode-dropdown-foreground);
      border: 1px solid var(--vscode-dropdown-border, rgba(128,128,128,0.35));
      border-radius: 3px;
      padding: 3px 6px;
      font-size: 12px;
      outline: none;
    }
    select.mode-select:focus {
      border-color: var(--vscode-focusBorder);
    }
    button {
      font-family: var(--vscode-font-family);
      font-size: 12px;
      border: none;
      border-radius: 3px;
      padding: 4px 10px;
      cursor: pointer;
      background: var(--vscode-button-background);
      color: var(--vscode-button-foreground);
    }
    button:hover:not(:disabled) {
      background: var(--vscode-button-hoverBackground);
    }
    button:disabled {
      opacity: 0.55;
      cursor: default;
    }
    button.secondary {
      background: var(--vscode-button-secondaryBackground);
      color: var(--vscode-button-secondaryForeground);
    }
    button.secondary:hover:not(:disabled) {
      background: var(--vscode-button-secondaryHoverBackground, var(--vscode-button-secondaryBackground));
    }
    button.danger {
      background: var(--vscode-errorForeground, #d73a49);
      color: #ffffff;
    }
    .plan-box {
      display: none;
      margin: 8px 10px 0;
      padding: 8px 10px;
      border-radius: 4px;
      background: var(--vscode-editorWidget-background, rgba(128,128,128,0.08));
      border: 1px solid var(--vscode-editorWidget-border, rgba(128,128,128,0.25));
      font-size: 12px;
    }
    .plan-box.visible {
      display: block;
    }
    .plan-header {
      font-weight: 600;
      margin-bottom: 4px;
      color: var(--vscode-descriptionForeground);
      display: flex;
      justify-content: space-between;
    }
    .plan-list {
      list-style: none;
      margin: 0;
      padding: 0;
    }
    .plan-item {
      display: flex;
      gap: 6px;
      padding: 2px 0;
      align-items: baseline;
    }
    .plan-item.completed {
      opacity: 0.72;
      text-decoration: line-through;
    }
    .timeline {
      flex: 1;
      overflow-y: auto;
      padding: 10px;
      display: flex;
      flex-direction: column;
      gap: 8px;
    }
    .empty-state {
      margin: auto 0;
      text-align: center;
      color: var(--vscode-descriptionForeground);
      padding: 18px 10px;
      line-height: 1.5;
    }
    .empty-state h3 {
      margin: 0 0 6px;
      color: var(--vscode-foreground);
      font-size: 14px;
    }
    .bubble {
      padding: 8px 10px;
      border-radius: 6px;
      line-height: 1.45;
      word-break: break-word;
    }
    .bubble.user {
      align-self: flex-end;
      max-width: 92%;
      background: var(--vscode-button-background);
      color: var(--vscode-button-foreground);
    }
    .bubble.user .ctx-badge {
      display: inline-block;
      font-size: 11px;
      opacity: 0.9;
      margin-bottom: 4px;
      padding: 1px 5px;
      border-radius: 3px;
      background: rgba(0, 0, 0, 0.2);
    }
    .bubble.assistant {
      background: var(--vscode-editor-background);
      border: 1px solid var(--vscode-panel-border, rgba(128,128,128,0.22));
    }
    .bubble pre {
      margin: 6px 0;
      padding: 8px;
      overflow-x: auto;
      border-radius: 4px;
      background: var(--vscode-textPreformat-background, rgba(128,128,128,0.14));
      font-family: var(--vscode-editor-font-family, monospace);
      font-size: 12px;
    }
    .bubble code {
      font-family: var(--vscode-editor-font-family, monospace);
      font-size: 12px;
      padding: 1px 4px;
      border-radius: 3px;
      background: var(--vscode-textPreformat-background, rgba(128,128,128,0.14));
    }
    .bubble pre code {
      padding: 0;
      background: transparent;
    }
    .tool-card, .perm-card, .error-card {
      padding: 8px 10px;
      border-radius: 5px;
      font-size: 12px;
      background: var(--vscode-editorWidget-background, rgba(128,128,128,0.08));
      border: 1px solid var(--vscode-editorWidget-border, rgba(128,128,128,0.25));
    }
    .error-card {
      border-color: var(--vscode-errorForeground, #d73a49);
      color: var(--vscode-errorForeground, #d73a49);
    }
    .tool-head {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 6px;
    }
    .tool-title {
      font-weight: 600;
      overflow: hidden;
      text-overflow: ellipsis;
      white-space: nowrap;
    }
    .badge {
      font-size: 10px;
      padding: 1px 5px;
      border-radius: 3px;
      text-transform: uppercase;
      background: var(--vscode-badge-background);
      color: var(--vscode-badge-foreground);
      flex-shrink: 0;
    }
    .tool-status {
      font-size: 11px;
      color: var(--vscode-descriptionForeground);
      flex-shrink: 0;
    }
    .tool-status.completed {
      color: var(--vscode-testing-iconPassed, #3fb950);
    }
    .tool-status.failed {
      color: var(--vscode-testing-iconFailed, #f85149);
    }
    .tool-actions {
      display: flex;
      flex-wrap: wrap;
      gap: 6px;
      margin-top: 6px;
    }
    .tool-output {
      margin-top: 6px;
      max-height: 140px;
      overflow: auto;
      padding: 6px;
      border-radius: 3px;
      font-family: var(--vscode-editor-font-family, monospace);
      font-size: 11px;
      white-space: pre-wrap;
      background: var(--vscode-textPreformat-background, rgba(128,128,128,0.12));
    }
    .perm-detail {
      margin: 6px 0;
      padding: 5px 7px;
      border-radius: 3px;
      font-family: var(--vscode-editor-font-family, monospace);
      font-size: 11px;
      word-break: break-all;
      background: var(--vscode-textPreformat-background, rgba(128,128,128,0.12));
    }
    .perm-buttons {
      display: flex;
      flex-wrap: wrap;
      gap: 6px;
      margin-top: 6px;
    }
    .composer {
      padding: 8px 10px 10px;
      border-top: 1px solid var(--vscode-panel-border, var(--vscode-widget-border, rgba(128,128,128,0.25)));
      display: flex;
      flex-direction: column;
      gap: 6px;
    }
    textarea {
      width: 100%;
      min-height: 58px;
      max-height: 160px;
      resize: vertical;
      padding: 7px 8px;
      border-radius: 4px;
      font-family: var(--vscode-font-family);
      font-size: var(--vscode-font-size);
      color: var(--vscode-input-foreground);
      background: var(--vscode-input-background);
      border: 1px solid var(--vscode-input-border, rgba(128,128,128,0.35));
      outline: none;
    }
    textarea:focus {
      border-color: var(--vscode-focusBorder);
    }
    .composer-bar {
      display: flex;
      justify-content: space-between;
      align-items: center;
      gap: 6px;
    }
    .hint {
      font-size: 11px;
      color: var(--vscode-descriptionForeground);
    }
  </style>
</head>
<body>
  <div class="toolbar">
    <div class="toolbar-left">
      <span id="statusDot" class="status-dot" title="Disconnected"></span>
      <select id="modeSelect" class="mode-select" aria-label="Approval Mode">
        <option value="ask">Mode: Ask</option>
        <option value="edit">Mode: Edit</option>
        <option value="full">Mode: Full</option>
        <option value="plan">Mode: Plan</option>
      </select>
    </div>
    <button id="newSessionBtn" class="secondary" type="button">New</button>
  </div>

  <div id="planBox" class="plan-box">
    <div class="plan-header">
      <span>Plan</span>
      <span id="planCount"></span>
    </div>
    <ul id="planList" class="plan-list"></ul>
  </div>

  <div id="timeline" class="timeline">
    <div id="emptyState" class="empty-state">
      <h3>DeskCode AI Coding Agent</h3>
      <div>Ask questions, edit workspace files with side-by-side diffs, or right-click code for <b>Ask About Selection</b> and <b>Fix Problems</b>.</div>
    </div>
  </div>

  <div class="composer">
    <textarea id="promptInput" placeholder="Ask DeskCode... (Enter to send, Shift+Enter for newline)"></textarea>
    <div class="composer-bar">
      <span class="hint" id="statusLabel">Ready</span>
      <div>
        <button id="stopBtn" class="danger" type="button" hidden>Stop</button>
        <button id="sendBtn" type="button">Send</button>
      </div>
    </div>
  </div>

  <script nonce="${nonce}">
    (function () {
      const vscode = acquireVsCodeApi();
      const timeline = document.getElementById("timeline");
      const emptyState = document.getElementById("emptyState");
      const modeSelect = document.getElementById("modeSelect");
      const newSessionBtn = document.getElementById("newSessionBtn");
      const promptInput = document.getElementById("promptInput");
      const sendBtn = document.getElementById("sendBtn");
      const stopBtn = document.getElementById("stopBtn");
      const statusDot = document.getElementById("statusDot");
      const statusLabel = document.getElementById("statusLabel");
      const planBox = document.getElementById("planBox");
      const planList = document.getElementById("planList");
      const planCount = document.getElementById("planCount");

      let running = false;
      let activeAssistantEl = null;
      let activeAssistantRaw = "";
      const toolCards = new Map();

      function escapeHtml(str) {
        return String(str)
          .replace(/&/g, "&amp;")
          .replace(/</g, "&lt;")
          .replace(/>/g, "&gt;")
          .replace(/"/g, "&quot;");
      }

      function renderMarkdownLite(raw) {
        const parts = String(raw).split(/\`\`\`([a-zA-Z0-9_-]*)\\n?([\\s\\S]*?)\`\`\`/g);
        let html = "";
        for (let i = 0; i < parts.length; i++) {
          if (i % 3 === 0) {
            let chunk = escapeHtml(parts[i]);
            chunk = chunk.replace(/\`([^\`\\n]+)\`/g, "<code>$1</code>");
            chunk = chunk.replace(/\\*\\*([^*\\n]+)\\*\\*/g, "<strong>$1</strong>");
            chunk = chunk.replace(/\\n/g, "<br>");
            html += chunk;
          } else if (i % 3 === 2) {
            html += "<pre><code>" + escapeHtml(parts[i]) + "</code></pre>";
          }
        }
        return html;
      }

      function hideEmpty() {
        if (emptyState) {
          emptyState.style.display = "none";
        }
      }

      function scrollToBottom() {
        timeline.scrollTop = timeline.scrollHeight;
      }

      function setRunningState(isRunning, sessionId) {
        running = Boolean(isRunning);
        sendBtn.hidden = running;
        stopBtn.hidden = !running;
        statusDot.className = "status-dot" + (running ? " running" : sessionId ? " connected" : "");
        statusLabel.textContent = running ? "DeskCode is working..." : "Ready";
      }

      function submitPrompt() {
        if (running) {
          return;
        }
        const text = promptInput.value.trim();
        if (!text) {
          return;
        }
        promptInput.value = "";
        vscode.postMessage({ type: "prompt", text });
      }

      sendBtn.addEventListener("click", submitPrompt);
      stopBtn.addEventListener("click", function () {
        vscode.postMessage({ type: "cancel" });
      });
      newSessionBtn.addEventListener("click", function () {
        vscode.postMessage({ type: "newSession" });
      });
      modeSelect.addEventListener("change", function () {
        vscode.postMessage({ type: "setMode", mode: modeSelect.value });
      });
      promptInput.addEventListener("keydown", function (e) {
        if (e.key === "Enter" && !e.shiftKey) {
          e.preventDefault();
          submitPrompt();
        }
      });

      timeline.addEventListener("click", function (e) {
        const btn = e.target.closest("button[data-action]");
        if (!btn || btn.disabled) {
          return;
        }
        const action = btn.getAttribute("data-action");
        if (action === "viewDiff") {
          vscode.postMessage({
            type: "viewDiff",
            diffId: btn.getAttribute("data-diff-id"),
          });
        } else if (action === "openLocation") {
          const lineAttr = btn.getAttribute("data-line");
          vscode.postMessage({
            type: "openLocation",
            path: btn.getAttribute("data-path"),
            line: lineAttr ? Number(lineAttr) : null,
          });
        } else if (action === "permissionReply") {
          vscode.postMessage({
            type: "permissionReply",
            requestId: btn.getAttribute("data-request-id"),
            optionId: btn.getAttribute("data-option-id"),
          });
        }
      });

      function ensureToolCard(toolCallId) {
        if (toolCards.has(toolCallId)) {
          return toolCards.get(toolCallId);
        }
        hideEmpty();
        activeAssistantEl = null;
        const card = document.createElement("div");
        card.className = "tool-card";
        card.innerHTML =
          '<div class="tool-head">' +
          '<span class="badge" data-role="kind">tool</span>' +
          '<span class="tool-title" data-role="title"></span>' +
          '<span class="tool-status" data-role="status"></span>' +
          "</div>" +
          '<div class="tool-actions" data-role="actions"></div>' +
          '<div class="tool-output" data-role="output" hidden></div>';
        timeline.appendChild(card);
        const record = { card };
        toolCards.set(toolCallId, record);
        scrollToBottom();
        return record;
      }

      function updateToolCard(data) {
        const rec = ensureToolCard(data.toolCallId);
        const card = rec.card;
        if (data.toolKind) {
          card.querySelector('[data-role="kind"]').textContent = data.toolKind;
        }
        if (data.title) {
          card.querySelector('[data-role="title"]').textContent = data.title;
        }
        if (data.status) {
          const st = card.querySelector('[data-role="status"]');
          st.textContent = data.status;
          st.className = "tool-status " + data.status;
        }
        const actions = card.querySelector('[data-role="actions"]');
        if (Array.isArray(data.locations) && data.locations.length > 0) {
          for (const loc of data.locations) {
            const key = "loc:" + loc.path + ":" + (loc.line || 0);
            if (!actions.querySelector('[data-key="' + CSS.escape(key) + '"]')) {
              const btn = document.createElement("button");
              btn.type = "button";
              btn.className = "secondary";
              btn.setAttribute("data-action", "openLocation");
              btn.setAttribute("data-key", key);
              btn.setAttribute("data-path", loc.path);
              if (loc.line) {
                btn.setAttribute("data-line", String(loc.line));
              }
              const shortName = loc.path.split(/[\\\\/]/).pop();
              btn.textContent = loc.line ? shortName + ":" + loc.line : shortName;
              actions.appendChild(btn);
            }
          }
        }
        if (Array.isArray(data.diffs) && data.diffs.length > 0) {
          for (const d of data.diffs) {
            const btn = document.createElement("button");
            btn.type = "button";
            btn.setAttribute("data-action", "viewDiff");
            btn.setAttribute("data-diff-id", d.diffId);
            btn.textContent = "View Diff (" + d.fileName + ")";
            actions.appendChild(btn);
          }
        }
        if (data.text) {
          const out = card.querySelector('[data-role="output"]');
          out.hidden = false;
          out.textContent = data.text;
        }
        scrollToBottom();
      }

      function renderPlan(entries) {
        if (!Array.isArray(entries) || entries.length === 0) {
          planBox.classList.remove("visible");
          planList.innerHTML = "";
          return;
        }
        planBox.classList.add("visible");
        planList.innerHTML = "";
        let done = 0;
        for (const item of entries) {
          if (item.status === "completed") {
            done++;
          }
          const li = document.createElement("li");
          li.className = "plan-item " + (item.status || "pending");
          const icon =
            item.status === "completed" ? "[x]" : item.status === "in_progress" ? "[>]" : "[ ]";
          li.textContent = icon + " " + item.content;
          planList.appendChild(li);
        }
        planCount.textContent = done + "/" + entries.length;
      }

      window.addEventListener("message", function (event) {
        const msg = event.data;
        if (!msg || typeof msg !== "object") {
          return;
        }
        switch (msg.type) {
          case "state":
            if (msg.mode) {
              modeSelect.value = msg.mode;
            }
            setRunningState(msg.running, msg.sessionId);
            break;
          case "reset":
            timeline.innerHTML = "";
            if (emptyState) {
              emptyState.style.display = "";
              timeline.appendChild(emptyState);
            }
            toolCards.clear();
            activeAssistantEl = null;
            activeAssistantRaw = "";
            renderPlan([]);
            if (msg.mode) {
              modeSelect.value = msg.mode;
            }
            setRunningState(false, null);
            break;
          case "modeChanged":
            if (msg.mode) {
              modeSelect.value = msg.mode;
            }
            break;
          case "userMessage": {
            hideEmpty();
            activeAssistantEl = null;
            activeAssistantRaw = "";
            const div = document.createElement("div");
            div.className = "bubble user";
            let inner = "";
            if (msg.contextLabel) {
              inner += '<div class="ctx-badge">' + escapeHtml(msg.contextLabel) + "</div><br>";
            }
            inner += escapeHtml(msg.text || "").replace(/\\n/g, "<br>");
            div.innerHTML = inner;
            timeline.appendChild(div);
            scrollToBottom();
            break;
          }
          case "turnStarted":
            setRunningState(true, true);
            break;
          case "assistantChunk":
            hideEmpty();
            if (!activeAssistantEl) {
              activeAssistantEl = document.createElement("div");
              activeAssistantEl.className = "bubble assistant";
              activeAssistantRaw = "";
              timeline.appendChild(activeAssistantEl);
            }
            activeAssistantRaw += msg.text || "";
            activeAssistantEl.innerHTML = renderMarkdownLite(activeAssistantRaw);
            scrollToBottom();
            break;
          case "toolCall":
          case "toolCallUpdate":
            updateToolCard(msg);
            break;
          case "plan":
            renderPlan(msg.entries);
            break;
          case "permissionRequest": {
            hideEmpty();
            activeAssistantEl = null;
            const card = document.createElement("div");
            card.className = "perm-card";
            card.id = "perm-" + msg.requestId;
            let html =
              '<div class="tool-head">' +
              '<span class="badge">' + escapeHtml(msg.toolKind || "approval") + "</span>" +
              '<span class="tool-title">' + escapeHtml(msg.title || "Approval required") + "</span>" +
              '<span class="tool-status" data-role="perm-outcome">pending</span>' +
              "</div>";
            if (msg.detail) {
              html += '<div class="perm-detail">' + escapeHtml(msg.detail) + "</div>";
            }
            html += '<div class="perm-buttons">';
            const opts = Array.isArray(msg.options) ? msg.options : [];
            for (const opt of opts) {
              const cls =
                opt.kind && opt.kind.indexOf("reject") === 0
                  ? "danger"
                  : opt.kind === "allow_always"
                  ? "secondary"
                  : "";
              html +=
                '<button type="button" class="' +
                cls +
                '" data-action="permissionReply" data-request-id="' +
                escapeHtml(msg.requestId) +
                '" data-option-id="' +
                escapeHtml(opt.optionId) +
                '">' +
                escapeHtml(opt.name || opt.optionId) +
                "</button>";
            }
            html += "</div>";
            card.innerHTML = html;
            timeline.appendChild(card);
            scrollToBottom();
            break;
          }
          case "permissionResolved": {
            const card = document.getElementById("perm-" + msg.requestId);
            if (card) {
              const buttons = card.querySelectorAll("button");
              buttons.forEach(function (b) {
                b.disabled = true;
              });
              const outcomeEl = card.querySelector('[data-role="perm-outcome"]');
              if (outcomeEl) {
                outcomeEl.textContent = msg.outcome || "resolved";
              }
            }
            break;
          }
          case "error": {
            hideEmpty();
            activeAssistantEl = null;
            const errDiv = document.createElement("div");
            errDiv.className = "error-card";
            errDiv.textContent = msg.message || "An unexpected error occurred.";
            timeline.appendChild(errDiv);
            scrollToBottom();
            break;
          }
          case "turnFinished":
            activeAssistantEl = null;
            activeAssistantRaw = "";
            setRunningState(false, true);
            if (msg.stopReason === "cancelled") {
              statusLabel.textContent = "Cancelled";
            }
            break;
        }
      });

      vscode.postMessage({ type: "ready" });
    })();
  </script>
</body>
</html>`;
  }
}

function formatSeverity(severity) {
  if (severity === vscode.DiagnosticSeverity.Error) {
    return "ERROR";
  }
  if (severity === vscode.DiagnosticSeverity.Warning) {
    return "WARNING";
  }
  return "INFO";
}

let activeProvider = null;

function activate(context) {
  const beforeProvider = new BeforeDocumentContentProvider();
  const chatProvider = new DeskCodeChatViewProvider(context, beforeProvider);
  activeProvider = chatProvider;

  context.subscriptions.push(
    beforeProvider,
    vscode.workspace.registerTextDocumentContentProvider(BEFORE_SCHEME, beforeProvider),
    vscode.window.registerWebviewViewProvider(VIEW_ID, chatProvider, {
      webviewOptions: { retainContextWhenHidden: true },
    }),
    vscode.commands.registerCommand("deskcode.newSession", async () => {
      await chatProvider.startNewSession();
    }),
    vscode.commands.registerCommand("deskcode.cancelTurn", () => {
      chatProvider.cancelCurrentTurn();
    }),
    vscode.commands.registerCommand("deskcode.setMode", async () => {
      const picked = await vscode.window.showQuickPick(
        MODE_DEFINITIONS.map((m) => ({
          label: m.label,
          description: m.id,
          detail: m.detail,
          modeId: m.id,
        })),
        {
          placeHolder: "Select DeskCode approval mode",
        }
      );
      if (picked && picked.modeId) {
        await chatProvider.switchMode(picked.modeId);
      }
    }),
    vscode.commands.registerCommand("deskcode.askSelection", async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor || editor.selection.isEmpty) {
        vscode.window.showInformationMessage("DeskCode: Select code in the active editor first.");
        return;
      }
      const selectedText = editor.document.getText(editor.selection);
      const userQuestion = await vscode.window.showInputBox({
        prompt: "Ask DeskCode about the selected code",
        placeHolder: "Explain this code, refactor it, or check for edge cases...",
      });
      if (userQuestion === undefined) {
        return;
      }
      const promptText = userQuestion.trim() || "Explain the selected code.";
      const startLine = editor.selection.start.line + 1;
      const endLine = editor.selection.end.line + 1;
      const baseUri = editor.document.uri.toString();
      const rangeUri = `${baseUri}#L${startLine}-L${endLine}`;
      const fileName = path.basename(editor.document.uri.fsPath || baseUri);
      await chatProvider.sendUserPrompt(
        promptText,
        [embeddedResourceBlock(rangeUri, selectedText, "text/plain")],
        `${fileName}:${startLine}-${endLine}`
      );
    }),
    vscode.commands.registerCommand("deskcode.fixDiagnostics", async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) {
        vscode.window.showInformationMessage("DeskCode: Open a file in the editor first.");
        return;
      }
      const docUri = editor.document.uri;
      const allDiagnostics = vscode.languages.getDiagnostics(docUri) || [];
      const relevant = allDiagnostics.filter(
        (d) =>
          d.severity === vscode.DiagnosticSeverity.Error ||
          d.severity === vscode.DiagnosticSeverity.Warning
      );
      if (relevant.length === 0) {
        vscode.window.showInformationMessage(
          "DeskCode: No errors or warnings found in the active file."
        );
        return;
      }
      const lines = relevant.slice(0, 50).map((d) => {
        const line = d.range.start.line + 1;
        const col = d.range.start.character + 1;
        const src = d.source ? `[${d.source}${d.code ? ` ${d.code}` : ""}] ` : "";
        return `- Line ${line}:${col} [${formatSeverity(d.severity)}] ${src}${d.message}`;
      });
      const fileName = path.basename(docUri.fsPath || docUri.toString());
      const promptText =
        `Fix the following diagnostics in ${fileName}:\n` + lines.join("\n");
      await chatProvider.sendUserPrompt(
        promptText,
        [resourceLinkBlock(docUri.toString(), fileName)],
        `${fileName} (${relevant.length} issue${relevant.length === 1 ? "" : "s"})`
      );
    }),
    vscode.workspace.onDidChangeConfiguration(async (event) => {
      if (
        event.affectsConfiguration("deskcode.executablePath") ||
        event.affectsConfiguration("deskcode.extraArgs")
      ) {
        await chatProvider.restartClient();
      }
    }),
    {
      dispose: () => {
        chatProvider.dispose();
      },
    }
  );

  return { chatProvider, beforeProvider };
}

async function deactivate() {
  if (activeProvider) {
    const provider = activeProvider;
    activeProvider = null;
    await provider.dispose();
  }
}

module.exports = {
  activate,
  deactivate,
  BEFORE_SCHEME,
  VIEW_ID,
  BeforeDocumentContentProvider,
  DeskCodeChatViewProvider,
};
