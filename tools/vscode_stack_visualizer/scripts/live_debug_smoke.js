#!/usr/bin/env node

const fs = require("fs");
const path = require("path");
const cp = require("child_process");
const assert = require("assert");

const extensionRoot = path.resolve(__dirname, "..");
const repoRoot = path.resolve(extensionRoot, "..", "..");
const interpreter = process.env.APC_COMPILER ||
    path.join(repoRoot, "build", "bin", "utxo_Interpreter");
const contract = path.join(
    repoRoot,
    "test",
    "debugger_regression",
    "debug_stack_visualizer_alt.ct"
);

if (!fs.existsSync(interpreter)) {
    console.log(`live debug smoke skipped: interpreter not found at ${interpreter}`);
    process.exit(0);
}

const child = cp.spawn(interpreter, [
    "debug-server",
    contract,
    "test_alt_roundtrip",
    "5"
], {
    cwd: repoRoot,
    stdio: ["pipe", "pipe", "pipe"]
});

let output = "";
let errorOutput = "";
child.stdout.on("data", (chunk) => {
    output += chunk;
});
child.stderr.on("data", (chunk) => {
    errorOutput += chunk;
});

child.stdin.write('{"seq":1,"command":"initialize"}\n');
child.stdin.write('{"seq":2,"command":"stepIn"}\n');
child.stdin.write('{"seq":3,"command":"stepIn"}\n');
child.stdin.write('{"seq":4,"command":"variables","scope":"instruction"}\n');
child.stdin.write('{"seq":5,"command":"disconnect"}\n');
child.stdin.end();

const timer = setTimeout(() => {
    child.kill("SIGTERM");
}, 5000);

child.on("exit", (code) => {
    clearTimeout(timer);
    if (code !== 0 && code !== null) {
        console.error(errorOutput || output || `debug server exited with ${code}`);
        process.exit(code);
    }
    try {
        const messages = output.trim().split(/\n+/).filter(Boolean).map((line) =>
            JSON.parse(line)
        );
        const failures = messages.filter((message) =>
            message.type === "response" && message.success === false
        );
        assert.deepStrictEqual(failures, [], "live debug responses should succeed");

        const step = messages.find((message) =>
            message.type === "response" &&
            message.command === "stepIn" &&
            message.body?.snapshot?.pc === 2
        );
        assert(step, "second stepIn response with pc=2 was not found");
        const snapshot = step.body.snapshot;
        assert.deepStrictEqual(
            snapshot.lineInstructions.map((item) => item.pc),
            [2, 3, 4],
            "pc=2 source line should expose every mapped instruction"
        );
        assert(snapshot.lineInstructionSummary.includes("pc 2*:"));
        assert(snapshot.lineInstructionSummary.includes("pc 3:"));
        assert(snapshot.lineInstructionSummary.includes("pc 4:"));

        const variablesResponse = messages.find((message) =>
            message.type === "response" && message.command === "variables"
        );
        assert(variablesResponse, "variables response was not found");
        const variables = Object.fromEntries(
            variablesResponse.body.variables.map((item) => [item.name, item.value])
        );
        assert.strictEqual(variables.opcode, snapshot.lineInstructionSummary);
        assert.strictEqual(variables.currentOpcode, snapshot.opcode);
    } catch (error) {
        console.error(error.stack || error.message);
        console.error(errorOutput || output);
        process.exit(1);
    }
    console.log("live debug smoke ok");
});
