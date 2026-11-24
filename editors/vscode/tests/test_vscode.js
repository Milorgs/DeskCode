#!/usr/bin/env node
"use strict";

const assert = require("assert");
const { execFileSync } = require("child_process");
const fs = require("fs");
const Module = require("module");
const os = require("os");
const path = require("path");

const VSCODE_DIR = path.resolve(__dirname, "..");
const REPO_ROOT = path.resolve(VSCODE_DIR, "..", "..");
const FAKE_AGENT = path.join(REPO_ROOT, "sdk", "python", "tests", "fake_acp_agent.py");
const REAL_DESK = path.join(REPO_ROOT, "build", "desk");

const {
  AcpClient,
  AcpError,
  AgentExitError,
  redactSecrets,
  textBlock,
  resourceLinkBlock,
  embeddedResourceBlock,
} = require("../src/acp_client");

function createFakeClient(tmpDir, options = {}) {
  const env = {
    PYTHONDONTWRITEBYTECODE: "1",
    ...(options.env || {}),
  };
  return new AcpClient({
    executablePath: "python3",
    extraArgs: ["-B", FAKE_AGENT],
    cwd: tmpDir,
    env,
    onPermission: options.onPermission,
    secrets: options.secrets,
    initTimeoutMs: options.initTimeoutMs || 10000,
  });
}

async function runTests() {
  const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "deskcode-vscode-test-"));
  let passed = 0;

  async function step(name, fn) {
    process.stdout.write(`  [RUN] ${name} ... `);
    await fn();
    passed++;
    process.stdout.write("OK\n");
  }

  try {
    await step("package.json and media/icon.svg manifest validation", async () => {
      const pkg = JSON.parse(
        fs.readFileSync(path.join(VSCODE_DIR, "package.json"), "utf8")
      );
      assert.strictEqual(pkg.name, "deskcode-vscode");
      assert.strictEqual(pkg.displayName, "DeskCode AI Coding Agent");
      assert.strictEqual(pkg.publisher, "milorgs");
      assert.strictEqual(pkg.version, "1.18.0");
      assert.strictEqual(pkg.engines.vscode, "^1.85.0");
      assert.strictEqual(pkg.main, "./src/extension.js");

      const container = pkg.contributes.viewsContainers.activitybar[0];
      assert.strictEqual(container.id, "deskcode");
      assert.strictEqual(container.icon, "media/icon.svg");

      const view = pkg.contributes.views.deskcode[0];
      assert.strictEqual(view.id, "deskcode.chatView");
      assert.strictEqual(view.name, "DeskCode");

      const commandIds = pkg.contributes.commands.map((c) => c.command);
      for (const expected of [
        "deskcode.newSession",
        "deskcode.cancelTurn",
        "deskcode.setMode",
        "deskcode.askSelection",
        "deskcode.fixDiagnostics",
      ]) {
        assert.ok(commandIds.includes(expected), `Missing command ${expected}`);
      }

      const contextCmds = pkg.contributes.menus["editor/context"].map((m) => m.command);
      assert.ok(contextCmds.includes("deskcode.askSelection"));
      assert.ok(contextCmds.includes("deskcode.fixDiagnostics"));

      const props = pkg.contributes.configuration.properties;
      assert.strictEqual(props["deskcode.executablePath"].default, "desk");
      assert.deepStrictEqual(props["deskcode.defaultMode"].enum, [
        "ask",
        "edit",
        "full",
        "plan",
      ]);
      assert.strictEqual(props["deskcode.defaultMode"].default, "edit");
      assert.deepStrictEqual(props["deskcode.extraArgs"].default, []);

      const svg = fs.readFileSync(path.join(VSCODE_DIR, "media", "icon.svg"), "utf8");
      assert.ok(svg.includes("<svg"));
      assert.ok(svg.includes("currentColor"));
    });

    await step("AcpClient handshake, newSession, setMode, and streaming chunks", async () => {
      const logFile = path.join(tmpDir, "acp_log.jsonl");
      const client = createFakeClient(tmpDir, {
        env: { FAKE_ACP_LOG: logFile },
      });
      try {
        const initRes = await client.start();
        assert.strictEqual(initRes.protocolVersion, 1);
        assert.strictEqual(client.agentInfo.name, "deskcode");

        const session = await client.newSession({ cwd: tmpDir, mode: "edit" });
        assert.ok(session.sessionId.startsWith("sess-"));
        assert.strictEqual(session.currentModeId, "edit");
        assert.strictEqual(session.availableModes.length, 4);

        await client.setMode(session.sessionId, "plan");
        await assert.rejects(
          () => client.setMode(session.sessionId, "invalid_mode"),
          (err) => err instanceof AcpError && err.code === -32602
        );

        const streamed = [];
        const turn = await client.prompt(session.sessionId, "hello", {
          onUpdate: (upd) => {
            if (upd.kind === "agent_message_chunk") {
              streamed.push(upd.text);
            }
          },
        });
        assert.deepStrictEqual(streamed, ["Hello", ", world"]);
        assert.strictEqual(turn.text, "Hello, world");
        assert.strictEqual(turn.stopReason, "end_turn");

        const logged = fs
          .readFileSync(logFile, "utf8")
          .trim()
          .split("\n")
          .map((line) => JSON.parse(line));
        assert.strictEqual(logged[0].method, "initialize");
        assert.strictEqual(logged[0].params.protocolVersion, 1);
        assert.deepStrictEqual(logged[0].params.clientInfo, {
          name: "deskcode-vscode",
          version: "1.17.0",
        });
      } finally {
        await client.close();
      }
    });

    await step("AcpClient tool_call, tool_call_update, diff, plan, and blocks", async () => {
      const client = createFakeClient(tmpDir);
      try {
        await client.start();
        const session = await client.newSession({ cwd: tmpDir, mode: "ask" });

        const turn = await client.prompt(session.sessionId, "tools");
        assert.strictEqual(turn.stopReason, "end_turn");
        assert.strictEqual(turn.text, "Done.");
        assert.strictEqual(turn.plan.length, 2);
        assert.strictEqual(turn.plan[0].content, "Read file");
        assert.strictEqual(turn.plan[0].status, "in_progress");

        assert.strictEqual(turn.toolCalls.length, 1);
        const tc = turn.toolCalls[0];
        assert.strictEqual(tc.toolCallId, "t1");
        assert.strictEqual(tc.status, "completed");
        assert.strictEqual(tc.text, "file body");
        assert.strictEqual(tc.locations.length, 1);
        assert.strictEqual(tc.locations[0].line, 3);
        assert.strictEqual(tc.diffs.length, 1);
        assert.strictEqual(tc.diffs[0].oldText, null);
        assert.strictEqual(tc.diffs[0].newText, "new");
        assert.strictEqual(tc.diffs[0].path, path.join(tmpDir, "a.txt"));

        // Mode update notification
        await client.prompt(session.sessionId, "mode");
        assert.strictEqual(client._sessions.get(session.sessionId).currentModeId, "plan");

        // Unknown agent->client method returns -32601
        const unk = await client.prompt(session.sessionId, "unknown");
        assert.strictEqual(unk.text, "error=-32601");

        // Echo prompt blocks (text + resource_link + embedded resource)
        const echoBlocks = [
          textBlock("echo"),
          resourceLinkBlock("file:///tmp/demo.cpp", "demo.cpp"),
          embeddedResourceBlock("file:///tmp/demo.cpp#L1-L3", "int main() {}", "text/x-c++"),
        ];
        const echoed = await client.prompt(session.sessionId, echoBlocks);
        const parsedBlocks = JSON.parse(echoed.text);
        assert.strictEqual(parsedBlocks.length, 3);
        assert.strictEqual(parsedBlocks[1].type, "resource_link");
        assert.strictEqual(parsedBlocks[2].type, "resource");
        assert.strictEqual(parsedBlocks[2].resource.text, "int main() {}");
      } finally {
        await client.close();
      }
    });

    await step("AcpClient session/request_permission round-trips", async () => {
      const client = createFakeClient(tmpDir);
      try {
        await client.start();
        const session = await client.newSession({ cwd: tmpDir, mode: "ask" });

        for (const [choice, expectedLabel, expectedStatus] of [
          ["allow", "allow", "completed"],
          ["allow_always", "allow_always", "completed"],
          ["reject", "reject", "failed"],
          [null, "cancelled", "failed"],
        ]) {
          const turn = await client.prompt(session.sessionId, "permission:execute", {
            onPermission: async (req) => {
              assert.strictEqual(req.toolCall.toolKind, "execute");
              assert.ok(req.options.some((o) => o.optionId === "allow"));
              return choice;
            },
          });
          assert.strictEqual(turn.text, `outcome=${expectedLabel}`);
          assert.strictEqual(turn.toolCalls[0].status, expectedStatus);
        }
      } finally {
        await client.close();
      }
    });

    await step("AcpClient session/cancel notification", async () => {
      const client = createFakeClient(tmpDir);
      try {
        await client.start();
        const session = await client.newSession({ cwd: tmpDir, mode: "edit" });

        const turn = await client.prompt(session.sessionId, "wait", {
          onUpdate: (upd) => {
            if (upd.kind === "agent_message_chunk" && upd.text === "working") {
              client.cancel(session.sessionId);
            }
          },
        });
        assert.strictEqual(turn.stopReason, "cancelled");
        assert.strictEqual(turn.text, "working");
      } finally {
        await client.close();
      }
    });

    await step("AcpClient crash handling, init error, and DESK_API_KEY redaction", async () => {
      const secretKey = "sk-vscode-secret-token-987654321";
      assert.strictEqual(
        redactSecrets(`err key=${secretKey} DESK_API_KEY=anotherSecret`, [secretKey]),
        "err key=*** DESK_API_KEY=***"
      );

      const crashClient = createFakeClient(tmpDir, {
        env: { DESK_API_KEY: secretKey },
      });
      try {
        await crashClient.start();
        const session = await crashClient.newSession({ cwd: tmpDir });
        await assert.rejects(
          () => crashClient.prompt(session.sessionId, "crash"),
          (err) => {
            assert.ok(err instanceof AgentExitError);
            assert.strictEqual(err.exitCode, 3);
            assert.ok(!err.message.includes(secretKey), "Leaked DESK_API_KEY in error message");
            assert.ok(!err.stderr.includes(secretKey), "Leaked DESK_API_KEY in stderr");
            assert.ok(err.message.includes("***"), "Expected redacted placeholder in message");
            return true;
          }
        );
        assert.ok(!crashClient.stderrText.includes(secretKey));
      } finally {
        await crashClient.close();
      }

      const initFailClient = createFakeClient(tmpDir, {
        env: { FAKE_ACP_EXIT_ON_INIT: "1" },
      });
      await assert.rejects(
        () => initFailClient.start(),
        (err) => err instanceof AgentExitError && err.message.includes("model not configured")
      );

      const missingBinClient = new AcpClient({
        executablePath: path.join(tmpDir, "nonexistent-desk-binary"),
        cwd: tmpDir,
      });
      await assert.rejects(
        () => missingBinClient.start(),
        (err) => err instanceof AgentExitError
      );
    });

    await step("VS Code extension provider, CSP, diff viewer, selection & diagnostics", async () => {
      const commands = new Map();
      const contentProviders = new Map();
      const webviewProviders = new Map();
      const executedDiffs = [];
      const postedToWebview = [];
      let webviewMessageHandler = null;
      let activeEditor = null;
      let diagnosticsList = [];
      let nextInputBoxReply = "Check edge cases";

      class MockEventEmitter {
        constructor() {
          this._listeners = [];
          this.event = (fn) => {
            this._listeners.push(fn);
            return { dispose: () => {} };
          };
        }
        fire(val) {
          for (const fn of this._listeners) {
            fn(val);
          }
        }
        dispose() {
          this._listeners = [];
        }
      }

      const makeUri = (scheme, fsPathOrPath) => ({
        scheme,
        path: fsPathOrPath,
        fsPath: fsPathOrPath,
        toString() {
          return `${scheme}://${fsPathOrPath}`;
        },
      });

      const mockVscode = {
        EventEmitter: MockEventEmitter,
        DiagnosticSeverity: { Error: 0, Warning: 1, Information: 2, Hint: 3 },
        Position: class {
          constructor(line, character) {
            this.line = line;
            this.character = character;
          }
        },
        Range: class {
          constructor(start, end) {
            this.start = start;
            this.end = end;
          }
        },
        Uri: {
          file: (p) => makeUri("file", p),
          parse: (raw) => {
            const idx = raw.indexOf(":");
            return makeUri(raw.slice(0, idx), raw.slice(idx + 1));
          },
        },
        workspace: {
          workspaceFolders: [{ uri: makeUri("file", tmpDir) }],
          getConfiguration: () => ({
            get: (key, fallback) => {
              if (key === "executablePath") return "python3";
              if (key === "extraArgs") return ["-B", FAKE_AGENT];
              if (key === "defaultMode") return "edit";
              return fallback;
            },
          }),
          registerTextDocumentContentProvider: (scheme, provider) => {
            contentProviders.set(scheme, provider);
            return { dispose: () => {} };
          },
          onDidChangeConfiguration: () => ({ dispose: () => {} }),
          openTextDocument: async (uri) => ({ uri }),
        },
        window: {
          get activeTextEditor() {
            return activeEditor;
          },
          registerWebviewViewProvider: (id, provider) => {
            webviewProviders.set(id, provider);
            return { dispose: () => {} };
          },
          showQuickPick: async (items) => items.find((i) => i.modeId === "plan"),
          showInputBox: async () => nextInputBoxReply,
          showInformationMessage: () => {},
          showWarningMessage: () => {},
          showTextDocument: async () => {},
        },
        languages: {
          getDiagnostics: () => diagnosticsList,
        },
        commands: {
          registerCommand: (id, handler) => {
            commands.set(id, handler);
            return { dispose: () => {} };
          },
          executeCommand: async (id, ...args) => {
            if (id === "vscode.diff") {
              executedDiffs.push(args);
              return;
            }
            if (commands.has(id)) {
              return commands.get(id)(...args);
            }
          },
        },
      };

      const origLoad = Module._load;
      Module._load = function (request, parent, isMain) {
        if (request === "vscode") {
          return mockVscode;
        }
        return origLoad.call(this, request, parent, isMain);
      };

      let ext;
      try {
        delete require.cache[require.resolve("../src/extension")];
        ext = require("../src/extension");
        const context = {
          extensionPath: VSCODE_DIR,
          subscriptions: [],
        };
        ext.activate(context);

        assert.ok(contentProviders.has("deskcode-before"));
        assert.ok(webviewProviders.has("deskcode.chatView"));
        assert.strictEqual(commands.size, 5);

        const chatProvider = webviewProviders.get("deskcode.chatView");
        const webviewView = {
          webview: {
            cspSource: "vscode-webview://test",
            options: {},
            html: "",
            onDidReceiveMessage: (fn) => {
              webviewMessageHandler = fn;
            },
            postMessage: (m) => {
              postedToWebview.push(m);
              if (m.type === "permissionRequest") {
                setImmediate(() => {
                  webviewMessageHandler({
                    type: "permissionReply",
                    requestId: m.requestId,
                    optionId: "allow",
                  });
                });
              }
            },
          },
          onDidDispose: () => {},
          show: () => {},
        };

        chatProvider.resolveWebviewView(webviewView);
        assert.ok(webviewView.webview.html.includes("Content-Security-Policy"));
        assert.ok(webviewView.webview.html.includes("nonce-"));
        assert.ok(webviewView.webview.html.includes("--vscode-"));

        await webviewMessageHandler({ type: "ready" });

        // Test tools + side-by-side diff via deskcode-before:
        const toolsRes = await chatProvider.sendUserPrompt("tools");
        assert.strictEqual(toolsRes.text, "Done.");
        const diffMsg = postedToWebview.find(
          (m) => m.type === "toolCallUpdate" && m.diffs && m.diffs.length > 0
        );
        assert.ok(diffMsg, "Expected toolCallUpdate with diff");
        await webviewMessageHandler({
          type: "viewDiff",
          diffId: diffMsg.diffs[0].diffId,
        });
        assert.strictEqual(executedDiffs.length, 1);
        const [beforeUri, _afterUri, diffTitle] = executedDiffs[0];
        assert.strictEqual(beforeUri.scheme, "deskcode-before");
        assert.ok(diffTitle.includes("a.txt"));
        assert.strictEqual(
          contentProviders.get("deskcode-before").provideTextDocumentContent(beforeUri),
          ""
        );

        // Test interactive permission button round-trip
        const permRes = await chatProvider.sendUserPrompt("permission:execute");
        assert.strictEqual(permRes.text, "outcome=allow");

        // Test askSelection command
        const sampleFile = path.join(tmpDir, "sample.cpp");
        fs.writeFileSync(sampleFile, "int add(int a, int b) {\n  return a + b;\n}\n");
        activeEditor = {
          document: {
            uri: mockVscode.Uri.file(sampleFile),
            getText: () => "return a + b;",
          },
          selection: {
            isEmpty: false,
            start: { line: 1, character: 2 },
            end: { line: 1, character: 15 },
          },
        };
        nextInputBoxReply = "echo Explain addition";
        postedToWebview.length = 0;
        await commands.get("deskcode.askSelection")();
        const assistantChunks = postedToWebview
          .filter((m) => m.type === "assistantChunk")
          .map((m) => m.text)
          .join("");
        const echoedSelection = JSON.parse(assistantChunks);
        assert.strictEqual(echoedSelection.length, 2);
        assert.strictEqual(echoedSelection[1].type, "resource");
        assert.strictEqual(echoedSelection[1].resource.text, "return a + b;");

        // Test fixDiagnostics command
        diagnosticsList = [
          {
            severity: 0,
            message: "undeclared identifier 'x'",
            source: "clangd",
            code: "E0020",
            range: { start: { line: 1, character: 9 } },
          },
        ];
        postedToWebview.length = 0;
        await commands.get("deskcode.fixDiagnostics")();
        assert.ok(postedToWebview.some((m) => m.type === "userMessage" && m.text.includes("undeclared identifier")));
      } finally {
        Module._load = origLoad;
        if (ext) {
          await ext.deactivate();
        }
      }
    });

    await step("Real ./build/desk binary check (when present)", async () => {
      if (!fs.existsSync(REAL_DESK)) {
        return;
      }
      if (process.env.DESK_VSCODE_TEST_REAL === "1") {
        const realClient = new AcpClient({
          executablePath: REAL_DESK,
          cwd: tmpDir,
          initTimeoutMs: 10000,
        });
        try {
          const init = await realClient.start();
          assert.strictEqual(init.protocolVersion, 1);
          const session = await realClient.newSession({ cwd: tmpDir, mode: "plan" });
          assert.ok(session.sessionId);
          assert.strictEqual(session.currentModeId, "plan");
        } catch (err) {
          // Unconfigured model environment exits cleanly with AgentExitError.
          assert.ok(err instanceof AgentExitError);
        } finally {
          await realClient.close();
        }
      } else {
        // Isolated unconfigured config dir verifies clean exit handling on real binary.
        const emptyCfg = path.join(tmpDir, "empty-cfg");
        fs.mkdirSync(emptyCfg, { recursive: true });
        const unconfigured = new AcpClient({
          executablePath: REAL_DESK,
          cwd: tmpDir,
          env: {
            HOME: emptyCfg,
            XDG_CONFIG_HOME: emptyCfg,
            DESK_API_KEY: "",
          },
          initTimeoutMs: 5000,
        });
        try {
          await unconfigured.start();
        } catch (err) {
          assert.ok(err instanceof AgentExitError);
        } finally {
          await unconfigured.close();
        }
      }
    });

    await step("package_vsix.py builds valid Open Packaging Conventions .vsix", async () => {
      const expectedVer = JSON.parse(
        fs.readFileSync(path.join(VSCODE_DIR, "package.json"), "utf8")
      ).version;
      const vsixPath = path.join(tmpDir, `deskcode-vscode-${expectedVer}.vsix`);
      const out = execFileSync(
        "python3",
        ["-B", path.join(VSCODE_DIR, "package_vsix.py"), vsixPath],
        {
          cwd: REPO_ROOT,
          env: { ...process.env, PYTHONDONTWRITEBYTECODE: "1" },
          encoding: "utf8",
        }
      ).trim();
      assert.strictEqual(out, vsixPath);
      assert.ok(fs.existsSync(vsixPath));

      const verifyScript = `
import json, sys, xml.etree.ElementTree as ET, zipfile
vsix, expected_ver = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(vsix, "r") as zf:
    assert zf.testzip() is None
    names = set(zf.namelist())
    required = {
        "[Content_Types].xml",
        "extension.vsixmanifest",
        "extension/package.json",
        "extension/README.md",
        "extension/src/extension.js",
        "extension/src/acp_client.js",
        "extension/media/icon.svg",
    }
    missing = required - names
    assert not missing, f"Missing entries: {missing}"
    for n in names:
        assert not n.startswith("extension/tests/"), f"Unexpected test file: {n}"
        assert n != "extension/package_vsix.py", "package_vsix.py should be excluded"
        assert not n.endswith(".vsix"), "Nested .vsix should be excluded"
    ct = ET.fromstring(zf.read("[Content_Types].xml"))
    exts = {el.attrib.get("Extension") for el in ct}
    assert {"json", "js", "md", "svg", "vsixmanifest"}.issubset(exts), exts
    mf = ET.fromstring(zf.read("extension.vsixmanifest"))
    xml_text = zf.read("extension.vsixmanifest").decode("utf-8")
    assert 'Id="deskcode-vscode"' in xml_text
    assert f'Version="{expected_ver}"' in xml_text
    assert 'Publisher="milorgs"' in xml_text
    pkg = json.loads(zf.read("extension/package.json").decode("utf-8"))
    assert pkg["version"] == expected_ver
print("VSIX_VALID")
`;
      const checkOut = execFileSync("python3", ["-B", "-c", verifyScript, vsixPath, expectedVer], {
        env: { ...process.env, PYTHONDONTWRITEBYTECODE: "1" },
        encoding: "utf8",
      }).trim();
      assert.strictEqual(checkOut, "VSIX_VALID");
    });

    console.log(`\nAll ${passed} VS Code extension test suites passed!`);
  } finally {
    fs.rmSync(tmpDir, { recursive: true, force: true });
  }
}

runTests().catch((err) => {
  console.error("\nTEST FAILED:", err);
  process.exit(1);
});
