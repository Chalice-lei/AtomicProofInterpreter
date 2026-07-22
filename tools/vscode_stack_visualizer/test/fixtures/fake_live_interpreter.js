#!/usr/bin/env node
"use strict";

const path = require("path");
const readline = require("readline");

const contractPath = process.argv[3] || "fixture.ct";
const crashAfterInitialize = path.basename(contractPath) === "crash.ct";
let pc = 0;

function value(number, id)
{
    return {
        hex: "0x" + Number(number).toString(16).padStart(2, "0"),
        intString: String(number),
        depth: 0,
        elementId: id
    };
}

function snapshot()
{
    return {
        pc,
        state: "paused",
        opcode: pc === 2 ? "OP_TOALTSTACK" : `OP_FAKE_${pc}`,
        operand: "",
        functionName: "fake_main",
        instructionCount: pc,
        range: {startPC: 0, endPC: 6},
        source: {file: contractPath, line: pc + 1, column: 1},
        lineInstructions: [{pc, opcode: `OP_FAKE_${pc}`, current: true}],
        lineInstructionSummary: `pc ${pc}*: OP_FAKE_${pc}`,
        mainStack: [value(10 + pc, `m${pc}`)],
        altStack: pc >= 2 ? [value(99, "alt-99")] : [],
        callStack: [{functionName: "fake_main", returnPC: 6}],
        warnings: ["fake warning"]
    };
}

function write(message)
{
    process.stdout.write(JSON.stringify(message) + "\n");
}

function response(request, body = {}, success = true, message = "")
{
    write({
        type: "response",
        request_seq: request.seq,
        command: request.command,
        success,
        body,
        ...(message ? {message} : {})
    });
}

write({type: "event", event: "ready", body: {snapshot: snapshot()}});

const reader = readline.createInterface({input: process.stdin});
reader.on("line", (line) => {
    const request = JSON.parse(line);
    if (request.command === "initialize") {
        response(request, {snapshot: snapshot()});
        if (crashAfterInitialize) {
            setTimeout(() => process.exit(17), 20);
        }
        return;
    }
    if (request.command === "setBreakpoints") {
        response(request, {
            breakpoints: (request.breakpoints || []).map((item, index) => ({
                id: index + 1,
                verified: item.line > 0 && item.line < 7,
                line: item.line,
                ...(item.line >= 7 ? {message: "line is not executable"} : {})
            })),
            snapshot: snapshot()
        });
        return;
    }
    if (["next", "stepIn", "stepOut", "continue", "pause"].includes(request.command)) {
        pc = request.command === "continue" ? 5 : Math.min(5, pc + 1);
        response(request, {
            allThreadsContinued: false,
            snapshot: snapshot()
        });
        write({
            type: "event",
            event: "stopped",
            body: {reason: request.command === "pause" ? "pause" : "step", snapshot: snapshot()}
        });
        return;
    }
    if (request.command === "evaluate") {
        const expression = String(request.expression || "");
        const values = {
            pc,
            opcode: snapshot().opcode,
            "main.length": snapshot().mainStack.length,
            "alt.length": snapshot().altStack.length,
            json: snapshot()
        };
        const valueResult = Object.prototype.hasOwnProperty.call(values, expression)
            ? values[expression]
            : expression;
        response(request, {
            result: typeof valueResult === "object" ? JSON.stringify(valueResult) : String(valueResult),
            value: valueResult,
            type: typeof valueResult
        });
        return;
    }
    if (request.command === "disconnect") {
        response(request, {snapshot: snapshot()});
        write({type: "event", event: "terminated", body: {snapshot: snapshot()}});
        reader.close();
        return;
    }
    response(request, {}, false, `unsupported ${request.command}`);
});
reader.on("close", () => setTimeout(() => process.exit(0), 5));
