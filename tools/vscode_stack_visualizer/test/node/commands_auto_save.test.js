"use strict";

const assert = require("assert");
const fs = require("fs");
const os = require("os");
const path = require("path");
const test = require("node:test");
const {edgeCaseTrace} = require("../helpers/trace_fixtures");
const {
    MockUri,
    createContext,
    createVscodeMock,
    loadExtensionWithMock
} = require("../helpers/vscode_mock");

const extensionRoot = path.resolve(__dirname, "..", "..");
const extensionPath = path.join(extensionRoot, "extension.js");
const fakeTraceInterpreter = path.join(
    extensionRoot,
    "test",
    "fixtures",
    "fake_trace_interpreter.js"
);

function setup(t, configuration = {})
{
    const workspaceRoot = fs.mkdtempSync(path.join(os.tmpdir(), "apc-commands-"));
    const contractA = path.join(workspaceRoot, "alpha.ct");
    const contractB = path.join(workspaceRoot, "beta.ct");
    fs.writeFileSync(contractA, "Contract Alpha:\n    def main(value: int):\n        Return(value)\n", "utf8");
    fs.writeFileSync(contractB, "Contract Beta:\n    def main(value: int):\n        Return(value)\n", "utf8");
    fs.writeFileSync(
        path.join(workspaceRoot, "run"),
        `process.argv.splice(2, 0, "run"); require(${JSON.stringify(fakeTraceInterpreter)});\n`,
        "utf8"
    );
    const tracePath = path.join(workspaceRoot, "valid.json");
    fs.writeFileSync(tracePath, JSON.stringify(edgeCaseTrace()), "utf8");
    const vscode = createVscodeMock({
        workspaceRoot,
        configuration: {
            interpreterPath: process.execPath,
            traceOutputPath: "${workspaceFolder}/${fileBasenameNoExtension}.trace.json",
            autoOpenGeneratedTrace: true,
            openBeside: true,
            "autoRunOnSave.enabled": false,
            "autoRunOnSave.mode": "trace",
            "autoRunOnSave.debounceMs": 20,
            "autoRunOnSave.restartLiveDebug": true,
            "autoRunOnSave.showStatus": false,
            ...configuration
        }
    });
    const context = createContext(extensionRoot);
    const extension = loadExtensionWithMock(extensionPath, vscode);
    extension.activate(context);
    t.after(() => {
        context.dispose();
        fs.rmSync(workspaceRoot, {recursive: true, force: true});
    });
    return {context, contractA, contractB, extension, tracePath, vscode, workspaceRoot};
}

function queueGenerateInputs(vscode, outputPath, functionName = "main", args = "5")
{
    vscode.__state.quickPick.push({label: functionName});
    vscode.__state.inputBox.push(args);
    vscode.__state.saveDialog.push(MockUri.file(outputPath));
}

function queueLiveInputs(vscode, functionName = "main", args = "5")
{
    vscode.__state.quickPick.push({label: functionName});
    vscode.__state.inputBox.push(args);
}

async function waitFor(predicate, message, timeoutMs = 3000)
{
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        if (predicate()) {
            return;
        }
        await new Promise((resolve) => setTimeout(resolve, 20));
    }
    throw new Error(message);
}

test("8 个公开命令均注册并完成正常路径", async (t) => {
    const {context, contractA, tracePath, vscode, workspaceRoot} = setup(t);
    const expected = [
        "openTrace",
        "openActiveTrace",
        "generateTrace",
        "openLastTrace",
        "debugActiveTrace",
        "debugLiveContract",
        "toggleAutoDebugOnSave",
        "restartCurrentLiveDebug"
    ].map((name) => `atomicProofStackVisualizer.${name}`);
    assert.deepStrictEqual([...vscode.__state.commandCallbacks.keys()], expected);

    vscode.__state.openDialog.push([MockUri.file(tracePath)]);
    await vscode.commands.executeCommand(expected[0]);
    assert.strictEqual(vscode.__state.panels.length, 1);

    await vscode.commands.executeCommand(expected[1], MockUri.file(tracePath));
    assert.strictEqual(vscode.__state.panels.length, 1, "same trace should reuse its panel");

    const generated = path.join(workspaceRoot, "generated.json");
    queueGenerateInputs(vscode, generated);
    await vscode.commands.executeCommand(expected[2], MockUri.file(contractA));
    assert(fs.existsSync(generated));
    assert.strictEqual(context.workspaceState.get("lastTracePath"), generated);

    await vscode.commands.executeCommand(expected[3]);
    assert(vscode.__state.panels.some((panel) => panel.title.includes("generated.json")));

    await vscode.commands.executeCommand(expected[4], MockUri.file(tracePath));
    assert.strictEqual(vscode.__state.debugStarts.at(-1).type, "atomicproof-trace");

    queueLiveInputs(vscode);
    await vscode.commands.executeCommand(expected[5], MockUri.file(contractA));
    assert.strictEqual(vscode.__state.debugStarts.at(-1).type, "atomicproof-live");

    await vscode.commands.executeCommand(expected[6]);
    assert.strictEqual(vscode.__state.configuration.get("autoRunOnSave.enabled"), true);

    const startsBeforeRestart = vscode.__state.debugStarts.length;
    await vscode.commands.executeCommand(expected[7]);
    assert.strictEqual(vscode.__state.debugStops.length, 1);
    assert.strictEqual(vscode.__state.debugStarts.length, startsBeforeRestart + 1);
});

test("8 个公开命令的取消/无上下文路径不产生副作用", async (t) => {
    const {contractA, vscode, workspaceRoot} = setup(t);
    const command = (name, ...args) => vscode.commands.executeCommand(
        `atomicProofStackVisualizer.${name}`,
        ...args
    );

    await command("openTrace");
    await command("openActiveTrace");
    await command("generateTrace");
    await command("openLastTrace");
    assert(vscode.__state.warnings.some((message) => message.includes("No generated")));

    const artifactSource = path.join(workspaceRoot, "artifact.ct");
    fs.writeFileSync(artifactSource, "Contract Artifact:\n    def main():\n", "utf8");
    const artifact = path.join(workspaceRoot, "artifact.json");
    fs.writeFileSync(artifact, JSON.stringify({
        lock: {asm: "OP_1", hex: "51"},
        functions: [{name: "main", params: []}]
    }), "utf8");
    await command("debugActiveTrace", MockUri.file(artifact));

    await command("debugLiveContract", MockUri.file(contractA));
    await command("restartCurrentLiveDebug");
    assert(vscode.__state.warnings.some((message) => message.includes("No AtomicProof live")));

    await command("toggleAutoDebugOnSave");
    await command("toggleAutoDebugOnSave");
    assert.strictEqual(vscode.__state.configuration.get("autoRunOnSave.enabled"), false);
    assert.strictEqual(vscode.__state.panels.length, 0);
    assert.strictEqual(vscode.__state.debugStarts.length, 0);
});

test("8 个公开命令的错误路径返回明确诊断", async (t) => {
    const {context, contractA, tracePath, vscode, workspaceRoot} = setup(t);
    const command = (name, ...args) => vscode.commands.executeCommand(
        `atomicProofStackVisualizer.${name}`,
        ...args
    );
    const invalid = path.join(workspaceRoot, "invalid.json");
    fs.writeFileSync(invalid, "{", "utf8");
    vscode.__state.openDialog.push([MockUri.file(invalid)]);
    await command("openTrace");
    assert.match(vscode.__state.errors.at(-1), /not valid JSON/);

    const wrong = path.join(workspaceRoot, "wrong.json");
    fs.writeFileSync(wrong, JSON.stringify({format: "old", steps: []}), "utf8");
    await command("openActiveTrace", MockUri.file(wrong));
    assert.match(vscode.__state.errors.at(-1), /not an apc-stack-trace/);

    vscode.__state.configuration.set("interpreterPath", path.join(workspaceRoot, "missing"));
    queueGenerateInputs(vscode, path.join(workspaceRoot, "never.json"));
    await command("generateTrace", MockUri.file(contractA));
    assert.match(vscode.__state.errors.at(-1), /Executable not found/);

    await context.workspaceState.update("lastTracePath", path.join(workspaceRoot, "missing.json"));
    await command("openLastTrace");
    assert.match(vscode.__state.errors.at(-1), /Failed to open stack trace/);

    await command("debugActiveTrace", MockUri.file(wrong));
    assert.match(vscode.__state.errors.at(-1), /Failed to start trace debugger/);

    vscode.__state.configuration.set("interpreterPath", process.execPath);
    vscode.__state.throwDebugStart = new Error("debug launch rejected");
    queueLiveInputs(vscode);
    await assert.rejects(command("debugLiveContract", MockUri.file(contractA)), /debug launch rejected/);

    vscode.__state.throwConfigUpdate = new Error("configuration is read-only");
    await assert.rejects(command("toggleAutoDebugOnSave"), /read-only/);
    vscode.__state.throwConfigUpdate = null;

    vscode.__state.throwDebugStart = null;
    queueLiveInputs(vscode);
    await command("debugLiveContract", MockUri.file(contractA));
    vscode.__state.throwDebugStart = new Error("restart rejected");
    await assert.rejects(command("restartCurrentLiveDebug"), /restart rejected/);
    assert(fs.existsSync(tracePath));
});

test("trace autoRunOnSave 对单合约防抖并隔离多合约输出", async (t) => {
    const {context, contractA, contractB, vscode, workspaceRoot} = setup(t, {
        "autoRunOnSave.enabled": true,
        "autoRunOnSave.mode": "trace",
        "autoRunOnSave.debounceMs": 25
    });
    await context.workspaceState.update("lastFunctionName", "main");
    await context.workspaceState.update("lastArguments", "5");

    vscode.workspace.__fireSave(MockUri.file(contractA));
    vscode.workspace.__fireSave(MockUri.file(contractA));
    vscode.workspace.__fireSave(MockUri.file(contractA));
    vscode.workspace.__fireSave(MockUri.file(contractB));

    const outputA = path.join(workspaceRoot, "alpha.trace.json");
    const outputB = path.join(workspaceRoot, "beta.trace.json");
    await waitFor(
        () => fs.existsSync(outputA) && fs.existsSync(outputB) &&
            (vscode.__state.output.join("").match(/\[Auto Trace\] Refreshed/g) || []).length === 2,
        "auto trace did not generate both contract outputs"
    );
    const refreshed = vscode.__state.output.join("").match(/\[Auto Trace\] Refreshed/g) || [];
    assert.strictEqual(refreshed.length, 2, "debounce should run once per contract");
});

test("live autoRunOnSave 防抖并分别重启两个合约会话", async (t) => {
    const {contractA, contractB, vscode} = setup(t, {
        "autoRunOnSave.enabled": true,
        "autoRunOnSave.mode": "live",
        "autoRunOnSave.debounceMs": 25
    });
    queueLiveInputs(vscode);
    await vscode.commands.executeCommand(
        "atomicProofStackVisualizer.debugLiveContract",
        MockUri.file(contractA)
    );
    queueLiveInputs(vscode);
    await vscode.commands.executeCommand(
        "atomicProofStackVisualizer.debugLiveContract",
        MockUri.file(contractB)
    );
    assert.strictEqual(vscode.__state.debugStarts.length, 2);

    vscode.workspace.__fireSave(MockUri.file(contractA));
    vscode.workspace.__fireSave(MockUri.file(contractA));
    vscode.workspace.__fireSave(MockUri.file(contractB));
    await waitFor(
        () => vscode.__state.debugStarts.length === 4,
        "auto live did not restart both contracts"
    );
    assert.strictEqual(vscode.__state.debugStops.length, 2);
    const restarted = vscode.__state.debugStarts.slice(2)
        .map((session) => path.basename(session.configuration.contractPath))
        .sort();
    assert.deepStrictEqual(restarted, ["alpha.ct", "beta.ct"]);
});

test("Webview 注入转义、nonce CSP 和命令参数注入均有回归保护", async (t) => {
    const {contractA, vscode, workspaceRoot} = setup(t);
    const marker = path.join(workspaceRoot, "command-injected");
    queueGenerateInputs(
        vscode,
        path.join(workspaceRoot, "safe.json"),
        "main",
        `\"; touch ${marker}; \" '$(uname)' \"带 空格\"`
    );
    await vscode.commands.executeCommand(
        "atomicProofStackVisualizer.generateTrace",
        MockUri.file(contractA)
    );
    assert.strictEqual(fs.existsSync(marker), false);

    const malicious = edgeCaseTrace();
    malicious.source.file = '</script><script>globalThis.__apcXss=1</script><script>';
    malicious.steps[0].opcode = '<img src=x onerror="globalThis.__apcXss=2">';
    const maliciousPath = path.join(workspaceRoot, "malicious.json");
    fs.writeFileSync(maliciousPath, JSON.stringify(malicious), "utf8");
    await vscode.commands.executeCommand(
        "atomicProofStackVisualizer.openActiveTrace",
        MockUri.file(maliciousPath)
    );
    const html = vscode.__state.panels.at(-1).webview.html;
    assert(!html.includes("</script><script>globalThis.__apcXss"));
    assert(html.includes("\\u003c/script>"));
    const csp = html.match(/Content-Security-Policy[^>]+/)[0];
    assert.match(csp, /default-src 'none'/);
    assert.match(csp, /script-src 'nonce-[A-Za-z0-9]{32}'/);
    assert(!/script-src[^>]*'unsafe-inline'/.test(csp));
    const nonce = csp.match(/script-src 'nonce-([A-Za-z0-9]{32})'/)[1];
    assert([...html.matchAll(/<script nonce="([^"]+)">/g)].every((match) => match[1] === nonce));

    vscode.__state.panels.at(-1).webview.__fireMessage({
        type: "openSource",
        file: "../../source-does-not-exist.ct",
        line: 1,
        column: 1
    });
    await new Promise((resolve) => setImmediate(resolve));
    assert.match(vscode.__state.warnings.at(-1), /Source file not found/);

    const externalRoot = fs.mkdtempSync(path.join(os.tmpdir(), "apc-external-source-"));
    const externalSource = path.join(externalRoot, "private.ct");
    fs.writeFileSync(externalSource, "Contract Private:\n", "utf8");
    t.after(() => fs.rmSync(externalRoot, {recursive: true, force: true}));
    const panel = vscode.__state.panels.at(-1);
    panel.webview.__fireMessage({
        type: "openSource",
        file: externalSource,
        line: 1,
        column: 1
    });
    await new Promise((resolve) => setImmediate(resolve));
    assert.match(vscode.__state.warnings.at(-1), /outside the workspace/);
    assert.strictEqual(vscode.__state.shownDocuments.length, 0);

    vscode.__state.warningChoice.push("Open Anyway");
    panel.webview.__fireMessage({
        type: "openSource",
        file: externalSource,
        line: 1,
        column: 1
    });
    await waitFor(
        () => vscode.__state.shownDocuments.length === 1,
        "确认后应打开受信任目录之外的源码"
    );
    assert.strictEqual(
        vscode.__state.shownDocuments[0].document.uri.fsPath,
        externalSource
    );
});
