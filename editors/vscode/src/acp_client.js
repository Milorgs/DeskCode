"use strict";

const { spawn } = require("child_process");
const { EventEmitter } = require("events");
const path = require("path");
const readline = require("readline");

const PROTOCOL_VERSION = 1;
const CLIENT_VERSION = "1.17.0";
const METHOD_NOT_FOUND = -32601;

class AcpError extends Error {
  constructor(code, message, data) {
    super(`ACP error ${code}: ${message}`);
    this.name = "AcpError";
    this.code = code;
    this.rpcMessage = message;
    this.data = data;
  }
}

class AgentExitError extends Error {
  constructor(message, { exitCode = null, signal = null, stderr = "" } = {}) {
    super(message);
    this.name = "AgentExitError";
    this.exitCode = exitCode;
    this.signal = signal;
    this.stderr = stderr;
  }
}

function redactSecrets(text, secrets = []) {
  if (!text) {
    return "";
  }
  let out = String(text);
  const validSecrets = Array.from(
    new Set(secrets.filter((s) => typeof s === "string" && s.length > 0))
  ).sort((a, b) => b.length - a.length);
  for (const secret of validSecrets) {
    out = out.split(secret).join("***");
  }
  out = out.replace(/(DESK_API_KEY\s*[=:]\s*)([^\s"'`]+)/gi, "$1***");
  return out;
}

function parseLocations(rawLocations) {
  if (!Array.isArray(rawLocations)) {
    return [];
  }
  const out = [];
  for (const item of rawLocations) {
    if (item && typeof item === "object" && typeof item.path === "string") {
      out.push({
        path: item.path,
        line: Number.isInteger(item.line) ? item.line : null,
      });
    }
  }
  return out;
}

function parseContents(rawContents) {
  if (!Array.isArray(rawContents)) {
    return { content: [], text: "", diffs: [] };
  }
  const content = [];
  const textParts = [];
  const diffs = [];
  for (const item of rawContents) {
    if (!item || typeof item !== "object") {
      continue;
    }
    if (item.type === "diff") {
      const diff = {
        type: "diff",
        path: String(item.path || ""),
        oldText: item.oldText === null || item.oldText === undefined ? null : String(item.oldText),
        newText: String(item.newText || ""),
      };
      content.push(diff);
      diffs.push(diff);
    } else if (
      item.type === "content" &&
      item.content &&
      typeof item.content === "object" &&
      item.content.type === "text"
    ) {
      const txt = String(item.content.text || "");
      content.push({ type: "text", text: txt });
      textParts.push(txt);
    } else {
      content.push(item);
    }
  }
  return { content, text: textParts.join(""), diffs };
}

function parseToolCall(raw) {
  const data = raw && typeof raw === "object" ? raw : {};
  const parsedContent = parseContents(data.content);
  return {
    toolCallId: String(data.toolCallId || ""),
    title: String(data.title || ""),
    toolKind: String(data.kind || "other"),
    status: String(data.status || "pending"),
    rawInput: data.rawInput !== undefined ? data.rawInput : null,
    rawOutput: data.rawOutput !== undefined ? data.rawOutput : null,
    locations: parseLocations(data.locations),
    content: parsedContent.content,
    text: parsedContent.text,
    diffs: parsedContent.diffs,
    raw: data,
  };
}

function parseSessionUpdate(update) {
  const data = update && typeof update === "object" ? update : {};
  const sessionUpdate = String(data.sessionUpdate || "");

  if (sessionUpdate === "agent_message_chunk") {
    const inner = data.content && typeof data.content === "object" ? data.content : {};
    const text = inner.type === "text" ? String(inner.text || "") : "";
    return {
      kind: "agent_message_chunk",
      text,
      raw: data,
    };
  }

  if (sessionUpdate === "tool_call") {
    return {
      kind: "tool_call",
      ...parseToolCall(data),
    };
  }

  if (sessionUpdate === "tool_call_update") {
    const parsedContent = parseContents(data.content);
    return {
      kind: "tool_call_update",
      toolCallId: String(data.toolCallId || ""),
      title: typeof data.title === "string" ? data.title : null,
      status: typeof data.status === "string" ? data.status : null,
      locations: parseLocations(data.locations),
      content: parsedContent.content,
      text: parsedContent.text,
      diffs: parsedContent.diffs,
      rawOutput: data.rawOutput !== undefined ? data.rawOutput : null,
      raw: data,
    };
  }

  if (sessionUpdate === "plan") {
    const rawEntries = Array.isArray(data.entries) ? data.entries : [];
    const entries = rawEntries
      .filter((e) => e && typeof e === "object")
      .map((e) => ({
        content: String(e.content || ""),
        priority: String(e.priority || "medium"),
        status: String(e.status || "pending"),
      }));
    return {
      kind: "plan",
      entries,
      raw: data,
    };
  }

  if (sessionUpdate === "current_mode_update") {
    return {
      kind: "current_mode_update",
      currentModeId: String(data.currentModeId || ""),
      raw: data,
    };
  }

  return {
    kind: "unknown",
    sessionUpdate,
    raw: data,
  };
}

function textBlock(text) {
  return { type: "text", text: String(text) };
}

function resourceLinkBlock(uri, name) {
  const block = { type: "resource_link", uri: String(uri) };
  if (name) {
    block.name = String(name);
  }
  return block;
}

function embeddedResourceBlock(uri, text, mimeType = "text/plain") {
  return {
    type: "resource",
    resource: {
      uri: String(uri),
      text: String(text),
      mimeType: String(mimeType),
    },
  };
}

class AcpClient extends EventEmitter {
  constructor(options = {}) {
    super();
    this.executablePath = options.executablePath || "desk";
    this.extraArgs = Array.isArray(options.extraArgs) ? options.extraArgs.map(String) : [];
    this.cwd = options.cwd ? path.resolve(options.cwd) : process.cwd();
    this.onPermission = typeof options.onPermission === "function" ? options.onPermission : null;
    this.clientInfo = options.clientInfo || {
      name: "deskcode-vscode",
      version: CLIENT_VERSION,
    };
    this.initTimeoutMs = Number.isFinite(options.initTimeoutMs) ? options.initTimeoutMs : 30000;

    this._fullEnv = { ...process.env, ...(options.env || {}) };
    const extraSecrets = Array.isArray(options.secrets) ? options.secrets : [];
    this._secrets = [
      this._fullEnv.DESK_API_KEY,
      process.env.DESK_API_KEY,
      ...extraSecrets,
    ].filter(Boolean);

    this.protocolVersion = null;
    this.agentCapabilities = {};
    this.agentInfo = {};
    this.authMethods = [];

    this._proc = null;
    this._rlOut = null;
    this._rlErr = null;
    this._nextId = 1;
    this._pending = new Map();
    this._sessions = new Map();
    this._activeTurns = new Map();
    this._stderrLines = [];
    this._closing = false;
    this._exitError = null;
    this._exitPromise = null;
  }

  get isRunning() {
    return Boolean(this._proc && this._exitError === null && !this._closing);
  }

  get stderrText() {
    return this._stderrLines.join("\n");
  }

  async start() {
    if (this._proc) {
      return {
        protocolVersion: this.protocolVersion,
        agentCapabilities: this.agentCapabilities,
        agentInfo: this.agentInfo,
        authMethods: this.authMethods,
      };
    }

    const args = [...this.extraArgs, "acp"];
    let proc;
    try {
      proc = spawn(this.executablePath, args, {
        cwd: this.cwd,
        env: this._fullEnv,
        stdio: ["pipe", "pipe", "pipe"],
        windowsHide: true,
      });
    } catch (err) {
      const msg = redactSecrets(
        `Failed to start '${this.executablePath}': ${err.message}`,
        this._secrets
      );
      throw new AgentExitError(msg, { stderr: "" });
    }

    this._proc = proc;
    this._closing = false;
    this._exitError = null;

    let stdoutDone = false;
    let stderrDone = false;
    let procClosed = false;
    let exitCode = null;
    let exitSignal = null;
    let spawnError = null;
    let resolveExit;
    this._exitPromise = new Promise((resolve) => {
      resolveExit = resolve;
    });

    const maybeFinalize = () => {
      if (spawnError || (stdoutDone && stderrDone && procClosed)) {
        this._finalizeExit(exitCode, exitSignal, spawnError);
        resolveExit();
      }
    };

    proc.on("error", (err) => {
      spawnError = err;
      maybeFinalize();
    });

    if (proc.stdin) {
      proc.stdin.on("error", () => {
        // Handled via process close / write callbacks.
      });
    }

    this._rlOut = readline.createInterface({
      input: proc.stdout,
      crlfDelay: Infinity,
    });
    this._rlErr = readline.createInterface({
      input: proc.stderr,
      crlfDelay: Infinity,
    });

    this._rlErr.on("line", (line) => {
      const clean = redactSecrets(line, this._secrets);
      this._stderrLines.push(clean);
      if (this._stderrLines.length > 200) {
        this._stderrLines.shift();
      }
      this.emit("stderr", clean);
    });

    this._rlOut.on("line", (line) => {
      const trimmed = line.trim();
      if (!trimmed) {
        return;
      }
      let msg;
      try {
        msg = JSON.parse(trimmed);
      } catch (_err) {
        return;
      }
      if (msg && typeof msg === "object" && !Array.isArray(msg)) {
        this._dispatch(msg);
      }
    });

    this._rlOut.on("close", () => {
      stdoutDone = true;
      maybeFinalize();
    });

    this._rlErr.on("close", () => {
      stderrDone = true;
      maybeFinalize();
    });

    proc.on("close", (code, signal) => {
      exitCode = code;
      exitSignal = signal;
      procClosed = true;
      maybeFinalize();
    });

    try {
      const result = await this.request(
        "initialize",
        {
          protocolVersion: PROTOCOL_VERSION,
          clientCapabilities: {
            fs: { readTextFile: false, writeTextFile: false },
            terminal: false,
          },
          clientInfo: this.clientInfo,
        },
        this.initTimeoutMs
      );
      this.protocolVersion = result ? result.protocolVersion : PROTOCOL_VERSION;
      this.agentCapabilities = (result && result.agentCapabilities) || {};
      this.agentInfo = (result && result.agentInfo) || {};
      this.authMethods = (result && result.authMethods) || [];
      return result;
    } catch (err) {
      await this.close();
      throw err;
    }
  }

  _finalizeExit(code, signal, spawnError) {
    if (this._exitError) {
      return;
    }
    const stderr = this.stderrText;
    const summary = stderr.length > 1500 ? `…${stderr.slice(-1500)}` : stderr.trim();
    let msg;
    if (spawnError) {
      msg = redactSecrets(
        `Failed to start '${this.executablePath}': ${spawnError.message}`,
        this._secrets
      );
    } else {
      const reason = code !== null ? `code ${code}` : `signal ${signal || "unknown"}`;
      msg = `desk acp exited with ${reason}${summary ? `: ${summary}` : ""}`;
    }

    const error = new AgentExitError(msg, {
      exitCode: code,
      signal,
      stderr,
    });
    this._exitError = error;

    const pendingEntries = Array.from(this._pending.values());
    this._pending.clear();
    for (const item of pendingEntries) {
      if (item.timer) {
        clearTimeout(item.timer);
      }
      item.reject(error);
    }

    this.emit("exit", error);
  }

  _send(payload) {
    if (!this._proc || !this._proc.stdin || this._proc.stdin.destroyed) {
      throw this._exitError || new AgentExitError("desk acp is not running");
    }
    if (this._exitError) {
      throw this._exitError;
    }
    const line = `${JSON.stringify(payload)}\n`;
    this._proc.stdin.write(line, "utf8");
  }

  request(method, params = {}, timeoutMs = 0) {
    if (this._exitError) {
      return Promise.reject(this._exitError);
    }
    const id = this._nextId++;
    const key = String(id);

    return new Promise((resolve, reject) => {
      const entry = { resolve, reject, timer: null };
      if (timeoutMs > 0) {
        entry.timer = setTimeout(() => {
          this._pending.delete(key);
          reject(new Error(`Timed out waiting for '${method}' response after ${timeoutMs}ms`));
        }, timeoutMs);
      }
      this._pending.set(key, entry);
      try {
        this._send({
          jsonrpc: "2.0",
          id,
          method,
          params: params || {},
        });
      } catch (err) {
        if (entry.timer) {
          clearTimeout(entry.timer);
        }
        this._pending.delete(key);
        reject(err);
      }
    });
  }

  notify(method, params = {}) {
    this._send({
      jsonrpc: "2.0",
      method,
      params: params || {},
    });
  }

  _respond(id, result = undefined, error = undefined) {
    if (!this._proc || !this._proc.stdin || this._proc.stdin.destroyed || this._exitError) {
      return;
    }
    const message = { jsonrpc: "2.0", id };
    if (error !== undefined) {
      message.error = error;
    } else {
      message.result = result;
    }
    try {
      this._send(message);
    } catch (_err) {
      // Ignore write failures while shutting down.
    }
  }

  _dispatch(message) {
    const hasMethod = typeof message.method === "string" && message.method.length > 0;
    const hasId = Object.prototype.hasOwnProperty.call(message, "id") && message.id !== null && message.id !== undefined;

    if (!hasMethod && hasId) {
      const key = String(message.id);
      const pending = this._pending.get(key);
      if (!pending) {
        return;
      }
      this._pending.delete(key);
      if (pending.timer) {
        clearTimeout(pending.timer);
      }
      if (message.error && typeof message.error === "object") {
        pending.reject(
          new AcpError(
            Number(message.error.code ?? -32603),
            redactSecrets(String(message.error.message || "Unknown RPC error"), this._secrets),
            message.error.data
          )
        );
      } else {
        pending.resolve(message.result);
      }
      return;
    }

    if (hasMethod && hasId) {
      if (message.method === "session/request_permission") {
        this._handlePermissionRequest(message.id, message.params || {});
      } else {
        this._respond(message.id, undefined, {
          code: METHOD_NOT_FOUND,
          message: `Method not found: ${message.method}`,
        });
      }
      return;
    }

    if (hasMethod && !hasId) {
      if (message.method === "session/update") {
        const params = message.params || {};
        const sessionId = String(params.sessionId || "");
        const parsed = parseSessionUpdate(params.update || {});
        const session = this._sessions.get(sessionId);
        if (session && parsed.kind === "current_mode_update" && parsed.currentModeId) {
          session.currentModeId = parsed.currentModeId;
        }
        const turn = this._activeTurns.get(sessionId);
        if (turn) {
          turn.onUpdate(parsed);
        }
        this.emit("sessionUpdate", { sessionId, update: parsed, raw: params.update });
      }
    }
  }

  async _handlePermissionRequest(requestId, params) {
    const sessionId = String(params.sessionId || "");
    const options = Array.isArray(params.options)
      ? params.options
          .filter((o) => o && typeof o === "object")
          .map((o) => ({
            optionId: String(o.optionId || ""),
            name: String(o.name || ""),
            kind: String(o.kind || ""),
          }))
      : [];

    const permissionRequest = {
      sessionId,
      toolCall: parseToolCall(params.toolCall || {}),
      options,
      raw: params,
    };

    const turn = this._activeTurns.get(sessionId);
    const handler = (turn && turn.onPermission) || this.onPermission;
    if (typeof handler !== "function") {
      this._respond(requestId, { outcome: { outcome: "cancelled" } });
      return;
    }

    let choice = null;
    try {
      choice = await handler(permissionRequest);
    } catch (_err) {
      choice = null;
    }

    const validIds = new Set(options.map((o) => o.optionId));
    if (typeof choice === "string" && validIds.has(choice)) {
      this._respond(requestId, {
        outcome: { outcome: "selected", optionId: choice },
      });
    } else {
      this._respond(requestId, {
        outcome: { outcome: "cancelled" },
      });
    }
  }

  async newSession({ cwd, mode, mcpServers = [] } = {}) {
    const resolvedCwd = path.resolve(cwd || this.cwd);
    const result = await this.request("session/new", {
      cwd: resolvedCwd,
      mcpServers: Array.isArray(mcpServers) ? mcpServers : [],
    });

    const sessionId = String(result.sessionId || "");
    const modesObj = (result && result.modes) || {};
    const availableModes = Array.isArray(modesObj.availableModes)
      ? modesObj.availableModes.map((m) => ({
          id: String(m.id || ""),
          name: String(m.name || ""),
          description: String(m.description || ""),
        }))
      : [];
    const session = {
      sessionId,
      cwd: resolvedCwd,
      currentModeId: String(modesObj.currentModeId || "ask"),
      availableModes,
    };
    this._sessions.set(sessionId, session);

    if (mode && mode !== session.currentModeId) {
      await this.setMode(sessionId, mode);
    }
    return { ...session };
  }

  async setMode(sessionId, modeId) {
    await this.request("session/set_mode", {
      sessionId: String(sessionId),
      modeId: String(modeId),
    });
    const session = this._sessions.get(String(sessionId));
    if (session) {
      session.currentModeId = String(modeId);
    }
  }

  cancel(sessionId) {
    this.notify("session/cancel", { sessionId: String(sessionId) });
  }

  async prompt(sessionId, promptInput, callbacks = {}) {
    const sid = String(sessionId);
    if (this._activeTurns.has(sid)) {
      throw new Error(`A prompt turn is already active on session ${sid}`);
    }

    const blocks =
      typeof promptInput === "string"
        ? [textBlock(promptInput)]
        : Array.isArray(promptInput)
        ? promptInput
        : [];
    if (blocks.length === 0) {
      throw new Error("Prompt requires at least one content block");
    }

    const textParts = [];
    const toolCalls = new Map();
    let plan = [];
    const events = [];

    const turnState = {
      onPermission: typeof callbacks.onPermission === "function" ? callbacks.onPermission : null,
      onUpdate: (parsed) => {
        events.push(parsed);
        if (parsed.kind === "agent_message_chunk") {
          textParts.push(parsed.text);
        } else if (parsed.kind === "tool_call") {
          toolCalls.set(parsed.toolCallId, { ...parsed });
        } else if (parsed.kind === "tool_call_update") {
          const existing = toolCalls.get(parsed.toolCallId) || {
            toolCallId: parsed.toolCallId,
            title: "",
            toolKind: "other",
            status: "pending",
            locations: [],
            content: [],
            text: "",
            diffs: [],
          };
          if (parsed.status) {
            existing.status = parsed.status;
          }
          if (parsed.title) {
            existing.title = parsed.title;
          }
          if (parsed.locations && parsed.locations.length > 0) {
            existing.locations = parsed.locations;
          }
          if (parsed.content && parsed.content.length > 0) {
            existing.content = parsed.content;
            existing.text = parsed.text;
            existing.diffs = parsed.diffs;
          }
          if (parsed.rawOutput !== null) {
            existing.rawOutput = parsed.rawOutput;
          }
          toolCalls.set(parsed.toolCallId, existing);
        } else if (parsed.kind === "plan") {
          plan = parsed.entries;
        }

        if (typeof callbacks.onUpdate === "function") {
          callbacks.onUpdate(parsed);
        }
      },
    };

    this._activeTurns.set(sid, turnState);
    try {
      const result = await this.request("session/prompt", {
        sessionId: sid,
        prompt: blocks,
      });
      return {
        stopReason: String((result && result.stopReason) || "end_turn"),
        text: textParts.join(""),
        toolCalls: Array.from(toolCalls.values()),
        plan,
        events,
      };
    } finally {
      this._activeTurns.delete(sid);
    }
  }

  async close(timeoutMs = 3000) {
    const proc = this._proc;
    if (!proc) {
      return;
    }
    this._closing = true;
    try {
      if (proc.stdin && !proc.stdin.destroyed) {
        proc.stdin.end();
      }
    } catch (_err) {
      // Ignore close errors.
    }

    if (this._exitPromise) {
      const exitedInTime = await Promise.race([
        this._exitPromise.then(() => true),
        new Promise((resolve) => setTimeout(() => resolve(false), timeoutMs)),
      ]);
      if (!exitedInTime && proc.exitCode === null) {
        try {
          proc.kill("SIGTERM");
        } catch (_err) {
          // Process already exited.
        }
        const terminatedInTime = await Promise.race([
          this._exitPromise.then(() => true),
          new Promise((resolve) => setTimeout(() => resolve(false), 1000)),
        ]);
        if (!terminatedInTime && proc.exitCode === null) {
          try {
            proc.kill("SIGKILL");
          } catch (_err) {
            // Process already exited.
          }
          await this._exitPromise;
        }
      }
    }

    if (this._rlOut) {
      this._rlOut.close();
      this._rlOut = null;
    }
    if (this._rlErr) {
      this._rlErr.close();
      this._rlErr = null;
    }
    this._proc = null;
  }
}

module.exports = {
  PROTOCOL_VERSION,
  CLIENT_VERSION,
  AcpClient,
  AcpError,
  AgentExitError,
  redactSecrets,
  parseSessionUpdate,
  textBlock,
  resourceLinkBlock,
  embeddedResourceBlock,
};
