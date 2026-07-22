#!/usr/bin/env node
"use strict";

const path = require("path");
const {runProcess} = require("../helpers/process");

const extensionRoot = path.resolve(__dirname, "..", "..");

(async () => {
    const result = await runProcess(process.execPath, [
        path.join(__dirname, "launch.js")
    ], {
        cwd: extensionRoot,
        timeoutMs: Number(process.env.APC_EXTENSION_TEST_TIMEOUT_MS || 90000),
        killTree: true
    });
    process.stdout.write(result.stdout);
    process.stderr.write(result.stderr);
    if (result.code !== 0) {
        process.exit(result.code || 1);
    }
})().catch((error) => {
    console.error(error.stack || error.message);
    process.exit(1);
});
