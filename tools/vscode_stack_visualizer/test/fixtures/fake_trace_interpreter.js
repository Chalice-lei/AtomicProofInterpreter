#!/usr/bin/env node
"use strict";

const fs = require("fs");

const args = process.argv.slice(2);
const outputIndex = args.indexOf("--stack-trace-output");
if (args.includes("compile_error")) {
    process.stderr.write("synthetic compile error\n");
    process.exit(2);
}
if (outputIndex < 0 || !args[outputIndex + 1]) {
    process.stderr.write("missing --stack-trace-output\n");
    process.exit(3);
}
const value = {
    hex: "0x01",
    intString: "1",
    elementId: "fake-e1",
    depth: 0,
    originStep: 0,
    originStack: "main"
};
fs.writeFileSync(args[outputIndex + 1], JSON.stringify({
    version: "1.0",
    format: "apc-stack-trace",
    source: {file: args[1], lines: ["Return(1)"]},
    steps: [{
        step: 0,
        pc: 0,
        opcode: "OP_1",
        sourceLine: 1,
        mainStackBefore: [],
        mainStackAfter: [value],
        altStackBefore: [],
        altStackAfter: [],
        effects: {
            mainStack: {pushed: [value], popped: [], sizeBefore: 0, sizeAfter: 1},
            altStack: {pushed: [], popped: [], sizeBefore: 0, sizeAfter: 0},
            moves: []
        }
    }]
}), "utf8");
