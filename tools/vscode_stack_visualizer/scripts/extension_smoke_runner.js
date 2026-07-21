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
    const liveVariables = extension.exports.__test.liveInstructionVariables({
        pc: 2,
        state: "paused",
        opcode: "OP_OVER",
        operand: "",
        functionName: "test_alt_roundtrip",
        instructionCount: 2,
        range: {startPC: 0, endPC: 11},
        source: {file: "/tmp/debug_stack_visualizer_alt.ct", line: 6},
        lineInstructionSummary: "pc 2*: OP_OVER; pc 3: OP_OVER; pc 4: OP_ADD"
    });
    const liveVariableMap = Object.fromEntries(
        liveVariables.map((item) => [item.name, item.value])
    );
    assert.strictEqual(
        liveVariableMap.opcode,
        "pc 2*: OP_OVER; pc 3: OP_OVER; pc 4: OP_ADD",
        "live opcode variable should show every opcode mapped to the source line"
    );
    assert.strictEqual(
        liveVariableMap.currentOpcode,
        "OP_OVER",
        "live currentOpcode variable should keep the current PC opcode"
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
