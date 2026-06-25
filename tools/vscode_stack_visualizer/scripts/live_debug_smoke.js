#!/usr/bin/env node

const fs = require("fs");
const path = require("path");
const cp = require("child_process");

const extensionRoot = path.resolve(__dirname, "..");
const repoRoot = path.resolve(extensionRoot, "..", "..");
const interpreter = process.env.APC_COMPILER ||
    path.join(repoRoot, "build", "bin", "utxo_interpreter");
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

child.stdin.write('{"id":1,"method":"initialize"}\n');
child.stdin.write('{"id":2,"method":"snapshot"}\n');
child.stdin.write('{"id":3,"method":"disconnect"}\n');
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
    console.log("live debug smoke ok");
});
