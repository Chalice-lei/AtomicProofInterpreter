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
if (args.includes("no_output")) {
    process.exit(0);
}
if (args.includes("invalid_output")) {
    fs.writeFileSync(args[outputIndex + 1], JSON.stringify({
        format: "apc-stack-trace",
        steps: [{step: "invalid", pc: 0}]
    }), "utf8");
    process.exit(0);
}
const value = {
    hex: "0x01",
    intString: "1",
    elementId: "fake-e1",
    depth: 0,
    originStep: 0,
    originStack: "main"
};
function writeTrace()
{
    fs.writeFileSync(args[outputIndex + 1], JSON.stringify({
        version: "1.0",
        format: "apc-stack-trace",
        source: {file: args[1], lines: ["Return(1)"]},
        invocationArguments: args.slice(3, outputIndex),
        steps: [{
            step: 0,
            pc: 0,
            opcode: "OP_1",
            functionName: args[2],
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
}

if (args.includes("slow_main")) {
    setTimeout(writeTrace, 150);
} else {
    writeTrace();
}
