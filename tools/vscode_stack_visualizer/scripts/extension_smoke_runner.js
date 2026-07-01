const assert = require("assert");
const fs = require("fs");
const path = require("path");
const vscode = require("vscode");

async function activateExtension()
{
    const extension = vscode.extensions.getExtension("atomicproof.atomicproof-stack-visualizer") ||
        vscode.extensions.all.find((item) =>
            item.packageJSON && item.packageJSON.name === "atomicproof-stack-visualizer"
        );
    assert(extension, "AtomicProof Stack Visualizer extension was not found");
    await extension.activate();
}

async function run()
{
    await activateExtension();
    const commands = await vscode.commands.getCommands(true);
    assert(
        commands.includes("atomicProofStackVisualizer.openTrace"),
        "openTrace command is not registered"
    );
    assert(
        commands.includes("atomicProofStackVisualizer.openActiveTrace"),
        "openActiveTrace command is not registered"
    );
    assert(
        commands.includes("atomicProofStackVisualizer.toggleAutoDebugOnSave"),
        "toggleAutoDebugOnSave command is not registered"
    );
    assert(
        commands.includes("atomicProofStackVisualizer.restartCurrentLiveDebug"),
        "restartCurrentLiveDebug command is not registered"
    );

    const extension = vscode.extensions.getExtension("atomicproof.atomicproof-stack-visualizer") ||
        vscode.extensions.all.find((item) =>
            item.packageJSON && item.packageJSON.name === "atomicproof-stack-visualizer"
        );
    const properties = extension.packageJSON.contributes.configuration.properties;
    assert.strictEqual(
        properties["atomicProofStackVisualizer.autoRunOnSave.enabled"].default,
        false,
        "autoRunOnSave.enabled default changed"
    );
    assert.deepStrictEqual(
        properties["atomicProofStackVisualizer.autoRunOnSave.mode"].enum,
        ["trace", "live"],
        "autoRunOnSave.mode enum is invalid"
    );
    assert.strictEqual(
        properties["atomicProofStackVisualizer.autoRunOnSave.debounceMs"].default,
        600,
        "autoRunOnSave.debounceMs default changed"
    );

    const schemaPath = path.resolve(
        __dirname,
        "..",
        "schemas",
        "apc-stack-trace.schema.json"
    );
    assert(fs.existsSync(schemaPath), "trace schema is missing");
}

module.exports = { run };
