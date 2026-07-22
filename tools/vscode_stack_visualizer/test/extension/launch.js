#!/usr/bin/env node
"use strict";

const fs = require("fs");
const os = require("os");
const path = require("path");
const {runTests} = require("@vscode/test-electron");

const extensionRoot = path.resolve(__dirname, "..", "..");
const repoRoot = path.resolve(extensionRoot, "..", "..");
const interpreter = process.env.APC_COMPILER || path.join(
    repoRoot,
    "build",
    "bin",
    process.platform === "win32" ? "utxo_Interpreter.exe" : "utxo_Interpreter"
);

async function main()
{
    if (!fs.existsSync(interpreter)) {
        throw new Error(`Core Extension Host test requires interpreter: ${interpreter}`);
    }
    delete process.env.ELECTRON_RUN_AS_NODE;
    const workspace = fs.mkdtempSync(path.join(os.tmpdir(), "apc-extension-host-"));
    try {
        fs.copyFileSync(
            path.join(repoRoot, "test", "debugger_regression", "debug_stack_visualizer_alt.ct"),
            path.join(workspace, "alpha.ct")
        );
        const trace = JSON.parse(fs.readFileSync(
            path.join(repoRoot, "examples", "stack_traces", "alt_roundtrip.json"),
            "utf8"
        ));
        trace.source.file = "alpha.ct";
        if (trace.debugInfo) {
            trace.debugInfo.sourceFile = "alpha.ct";
        }
        for (const step of trace.steps) {
            step.sourceFile = "alpha.ct";
            if (step.source) step.source.file = "alpha.ct";
        }
        fs.writeFileSync(
            path.join(workspace, "trace.json"),
            JSON.stringify(trace),
            "utf8"
        );
        process.env.APC_TEST_WORKSPACE = workspace;
        process.env.APC_COMPILER = interpreter;
        await runTests({
            extensionDevelopmentPath: extensionRoot,
            extensionTestsPath: path.join(__dirname, "runner.js"),
            launchArgs: [workspace, "--disable-extensions", "--disable-gpu"]
        });
    } finally {
        fs.rmSync(workspace, {recursive: true, force: true});
    }
}

main().catch((error) => {
    console.error(error.stack || error.message);
    process.exit(1);
});
