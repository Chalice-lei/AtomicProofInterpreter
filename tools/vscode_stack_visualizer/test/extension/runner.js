"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const vscode = require("vscode");

async function waitFor(predicate, message, timeoutMs = 15000)
{
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        const value = predicate();
        if (value) return value;
        await new Promise((resolve) => setTimeout(resolve, 50));
    }
    throw new Error(message);
}

async function makeDocumentDirtyAndSave(uri, suffix)
{
    const document = await vscode.workspace.openTextDocument(uri);
    const lastLine = Math.max(0, document.lineCount - 1);
    const position = new vscode.Position(lastLine, document.lineAt(lastLine).text.length);
    const edit = new vscode.WorkspaceEdit();
    edit.insert(uri, position, suffix);
    assert.strictEqual(await vscode.workspace.applyEdit(edit), true);
    assert.strictEqual(await document.save(), true);
}

async function run()
{
    const workspace = process.env.APC_TEST_WORKSPACE;
    const interpreter = process.env.APC_COMPILER;
    assert(workspace && fs.existsSync(workspace), "test workspace is missing");
    assert(interpreter && fs.existsSync(interpreter), "interpreter is missing");

    const extension = vscode.extensions.getExtension("atomicproof.atomicproof-stack-visualizer") ||
        vscode.extensions.all.find((item) =>
            item.packageJSON?.name === "atomicproof-stack-visualizer"
        );
    assert(extension, "AtomicProof extension was not found");
    await extension.activate();

    const expectedCommands = [
        "openTrace",
        "openActiveTrace",
        "generateTrace",
        "openLastTrace",
        "debugActiveTrace",
        "debugLiveContract",
        "toggleAutoDebugOnSave",
        "restartCurrentLiveDebug"
    ].map((name) => `atomicProofStackVisualizer.${name}`);
    const commands = await vscode.commands.getCommands(true);
    for (const command of expectedCommands) {
        assert(commands.includes(command), `${command} is not registered`);
    }

    const traceUri = vscode.Uri.file(path.join(workspace, "trace.json"));
    await vscode.commands.executeCommand(
        "atomicProofStackVisualizer.openActiveTrace",
        traceUri
    );
    await waitFor(
        () => vscode.window.tabGroups.all.flatMap((group) => group.tabs)
            .some((tab) => String(tab.label).includes("trace.json")),
        "public openActiveTrace command did not open a Webview"
    );

    const started = [];
    const terminated = [];
    const startSubscription = vscode.debug.onDidStartDebugSession((session) => started.push(session));
    const terminateSubscription = vscode.debug.onDidTerminateDebugSession((session) => terminated.push(session));
    try {
        await vscode.commands.executeCommand(
            "atomicProofStackVisualizer.debugActiveTrace",
            traceUri
        );
        const traceSession = await waitFor(
            () => started.find((session) => session.type === "atomicproof-trace"),
            "public debugActiveTrace command did not start Trace DAP"
        );
        assert.strictEqual(await traceSession.customRequest("evaluate", {expression: "pc"}).then((x) => x.result), "0");
        await traceSession.customRequest("next");
        assert.strictEqual(await traceSession.customRequest("evaluate", {expression: "pc"}).then((x) => x.result), "1");
        await vscode.debug.stopDebugging(traceSession);
        await waitFor(() => terminated.includes(traceSession), "Trace DAP did not terminate");

        const config = vscode.workspace.getConfiguration("atomicProofStackVisualizer");
        await config.update("interpreterPath", interpreter, vscode.ConfigurationTarget.Workspace);
        await config.update("defaultFunction", "test_alt_roundtrip", vscode.ConfigurationTarget.Workspace);
        await config.update("defaultArguments", "5", vscode.ConfigurationTarget.Workspace);
        await config.update(
            "traceOutputPath",
            "${workspaceFolder}/${fileBasenameNoExtension}.auto.json",
            vscode.ConfigurationTarget.Workspace
        );
        await config.update("autoRunOnSave.debounceMs", 50, vscode.ConfigurationTarget.Workspace);
        await config.update("autoRunOnSave.showStatus", false, vscode.ConfigurationTarget.Workspace);
        await config.update("autoRunOnSave.mode", "trace", vscode.ConfigurationTarget.Workspace);
        await config.update("autoRunOnSave.enabled", true, vscode.ConfigurationTarget.Workspace);

        const contractUri = vscode.Uri.file(path.join(workspace, "alpha.ct"));
        await makeDocumentDirtyAndSave(contractUri, " ");
        const generatedTrace = path.join(workspace, "alpha.auto.json");
        await waitFor(() => fs.existsSync(generatedTrace), "trace autoRunOnSave did not generate output");
        const generated = JSON.parse(fs.readFileSync(generatedTrace, "utf8"));
        assert.strictEqual(generated.format, "apc-stack-trace");
        assert(generated.steps.length > 0);

        await config.update("autoRunOnSave.mode", "live", vscode.ConfigurationTarget.Workspace);
        const liveConfig = {
            type: "atomicproof-live",
            request: "launch",
            name: "Extension Host Live Test",
            contractPath: contractUri.fsPath,
            functionName: "test_alt_roundtrip",
            arguments: ["5"],
            interpreterPath: interpreter
        };
        assert.strictEqual(await vscode.debug.startDebugging(undefined, liveConfig), true);
        const firstLive = await waitFor(
            () => started.find((session) => session.type === "atomicproof-live"),
            "Live DAP did not start"
        );
        await firstLive.customRequest("next");
        assert.strictEqual(await firstLive.customRequest("evaluate", {expression: "pc"}).then((x) => x.result), "1");
        await makeDocumentDirtyAndSave(contractUri, " ");
        const restartedLive = await waitFor(
            () => started.find((session) =>
                session.type === "atomicproof-live" && session.id !== firstLive.id
            ),
            "live autoRunOnSave did not restart the matching session"
        );
        assert(terminated.some((session) => session.id === firstLive.id));
        await restartedLive.customRequest("terminate");
        await waitFor(
            () => terminated.some((session) => session.id === restartedLive.id),
            "Live DAP terminate did not end Extension Host session"
        );

        await vscode.commands.executeCommand("atomicProofStackVisualizer.toggleAutoDebugOnSave");
        assert.strictEqual(config.get("autoRunOnSave.enabled"), false);
        await vscode.commands.executeCommand("atomicProofStackVisualizer.restartCurrentLiveDebug");
        console.log("Extension Host integration assertions ok");
    } finally {
        startSubscription.dispose();
        terminateSubscription.dispose();
        await vscode.debug.stopDebugging();
    }
}

module.exports = {run};
