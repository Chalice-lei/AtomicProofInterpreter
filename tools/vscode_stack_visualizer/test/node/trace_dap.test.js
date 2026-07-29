"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const test = require("node:test");
const {assertSuccess, attach} = require("../helpers/dap_client");
const {
    createVscodeMock,
    loadExtensionWithMock
} = require("../helpers/vscode_mock");

const extensionRoot = path.resolve(__dirname, "..", "..");
const repoRoot = path.resolve(extensionRoot, "..", "..");
const extensionPath = path.join(extensionRoot, "extension.js");
const arithmeticTrace = path.join(
    repoRoot,
    "examples",
    "stack_traces",
    "arithmetic_line_mapping.json"
);

function createAdapter()
{
    const vscode = createVscodeMock({workspaceRoot: repoRoot});
    const extension = loadExtensionWithMock(extensionPath, vscode);
    return new extension.__test.AtomicProofTraceDebugAdapter();
}

test("Trace DAP 支持 initialize、launch、next、stepBack 和 source", async (t) => {
    const client = attach(createAdapter());
    t.after(() => client.dispose());

    const capabilities = assertSuccess(assert, await client.request("initialize"));
    assert.strictEqual(capabilities.supportsStepBack, true);
    assert.strictEqual(capabilities.supportsConditionalBreakpoints, true);
    assert.strictEqual(capabilities.supportsHitConditionalBreakpoints, true);

    assertSuccess(assert, await client.request("launch", {tracePath: arithmeticTrace}));
    assert.strictEqual(client.events("initialized").length, 1);
    assertSuccess(assert, await client.request("configurationDone"));
    assert.strictEqual(client.events("stopped").at(-1).body.reason, "entry");

    assertSuccess(assert, await client.request("next"));
    assert.strictEqual(client.events("stopped").at(-1).body.reason, "step");
    assert.strictEqual(
        assertSuccess(assert, await client.request("evaluate", {expression: "pc"})).result,
        "1"
    );
    assertSuccess(assert, await client.request("stepBack"));
    assert.strictEqual(
        assertSuccess(assert, await client.request("evaluate", {expression: "pc"})).result,
        "0"
    );

    const source = assertSuccess(assert, await client.request("source", {
        source: {path: "/missing/debug_line_mapping_basic.ct"}
    }));
    assert(source.content.includes("Contract DebugLineMappingBasic"));
});

test("Trace DAP 的 continue/reverseContinue 命中普通、条件和 hit count 断点", async (t) => {
    const client = attach(createAdapter());
    t.after(() => client.dispose());
    assertSuccess(assert, await client.request("launch", {tracePath: arithmeticTrace}));

    const sourcePath = path.join(
        repoRoot,
        "test",
        "debugger_regression",
        "debug_line_mapping_basic.ct"
    );
    const wrongSource = assertSuccess(assert, await client.request("setBreakpoints", {
        source: {path: path.join(path.dirname(sourcePath), "different.ct")},
        breakpoints: [{line: 8}]
    }));
    assert.strictEqual(wrongSource.breakpoints[0].verified, false);

    const breakpointBody = assertSuccess(assert, await client.request("setBreakpoints", {
        source: {path: sourcePath},
        breakpoints: [{line: 8, condition: "pc >= 2", hitCondition: "2"}]
    }));
    assert.strictEqual(breakpointBody.breakpoints[0].verified, true);
    assertSuccess(assert, await client.request("continue"));
    assert.strictEqual(client.events("stopped").at(-1).body.reason, "breakpoint");
    assert.strictEqual(
        assertSuccess(assert, await client.request("evaluate", {expression: "pc"})).result,
        "2"
    );

    assertSuccess(assert, await client.request("setBreakpoints", {
        source: {path: sourcePath},
        breakpoints: [{line: 9}]
    }));
    assertSuccess(assert, await client.request("continue"));
    assert.strictEqual(
        assertSuccess(assert, await client.request("evaluate", {expression: "line"})).result,
        "9"
    );
    assertSuccess(assert, await client.request("next"));
    assertSuccess(assert, await client.request("reverseContinue"));
    assert.strictEqual(client.events("stopped").at(-1).body.reason, "breakpoint");
    assert.strictEqual(
        assertSuccess(assert, await client.request("evaluate", {expression: "line"})).result,
        "9"
    );

    const unverified = assertSuccess(assert, await client.request("setBreakpoints", {
        source: {path: sourcePath},
        breakpoints: [{line: 9999}]
    }));
    assert.strictEqual(unverified.breakpoints[0].verified, false);
    assert.match(unverified.breakpoints[0].message, /No trace step/);
});

test("Trace DAP logpoint 只输出不暂停，并安全插值表达式", async (t) => {
    const client = attach(createAdapter());
    t.after(() => client.dispose());
    assertSuccess(assert, await client.request("launch", {tracePath: arithmeticTrace}));
    assertSuccess(assert, await client.request("setBreakpoints", {
        source: {path: "debug_line_mapping_basic.ct"},
        breakpoints: [{line: 8, logMessage: "pc={pc} opcode={opcode}"}]
    }));
    assertSuccess(assert, await client.request("continue"));
    assert.strictEqual(client.events("stopped").at(-1).body.reason, "end");
    const output = client.events("output").map((event) => event.body.output).join("");
    assert.match(output, /pc=1 opcode=OP_ROT/);
    assert.match(output, /pc=2 opcode=OP_ADD/);
});

test("Trace DAP variables 和 evaluate 覆盖 instruction/main/alt/effects/json", async (t) => {
    const client = attach(createAdapter());
    t.after(() => client.dispose());
    assertSuccess(assert, await client.request("launch", {tracePath: arithmeticTrace}));
    assertSuccess(assert, await client.request("next"));
    assertSuccess(assert, await client.request("next"));

    const scopes = assertSuccess(assert, await client.request("scopes", {frameId: 1})).scopes;
    assert.deepStrictEqual(
        scopes.map((scope) => scope.name),
        ["Instruction", "Main Stack After", "Alt Stack After", "Effects"]
    );
    for (const scope of scopes) {
        const body = assertSuccess(assert, await client.request("variables", {
            variablesReference: scope.variablesReference
        }));
        assert(Array.isArray(body.variables));
    }

    assert.strictEqual(
        assertSuccess(assert, await client.request("evaluate", {expression: "main.length"})).result,
        "2"
    );
    assert.match(
        assertSuccess(assert, await client.request("evaluate", {expression: "main[0].hex"})).result,
        /^0x/
    );
    assert.match(
        assertSuccess(assert, await client.request("evaluate", {expression: "json"})).result,
        /"opcode":"OP_ADD"/
    );
    const effects = assertSuccess(assert, await client.request("evaluate", {
        expression: "effects.moves"
    }));
    assert.strictEqual(effects.result, "Array(0)");
});

test("Trace DAP 对缺路径、非法 JSON、错误 format 和 disconnect 明确响应", async (t) => {
    const client = attach(createAdapter());
    t.after(() => client.dispose());
    const missing = await client.request("launch", {});
    assert.strictEqual(missing.success, false);
    assert.match(missing.message, /missing tracePath/);

    const tempDir = fs.mkdtempSync(path.join(require("os").tmpdir(), "apc-trace-dap-"));
    t.after(() => fs.rmSync(tempDir, {recursive: true, force: true}));
    const invalidJson = path.join(tempDir, "invalid.json");
    fs.writeFileSync(invalidJson, "{", "utf8");
    const invalidResponse = await client.request("launch", {tracePath: invalidJson});
    assert.strictEqual(invalidResponse.success, false);

    const wrongFormat = path.join(tempDir, "wrong.json");
    fs.writeFileSync(wrongFormat, JSON.stringify({format: "old", steps: []}), "utf8");
    const wrongResponse = await client.request("launch", {tracePath: wrongFormat});
    assert.strictEqual(wrongResponse.success, false);
    assert.match(wrongResponse.message, /not an apc-stack-trace/);

    assertSuccess(assert, await client.request("launch", {tracePath: arithmeticTrace}));
    assertSuccess(assert, await client.request("disconnect"));
    assert.strictEqual(client.events("terminated").length, 1);
});

test("Trace DAP 按源码保留断点且 imported source 不返回主文件嵌入内容", async (t) => {
    const tempDir = fs.mkdtempSync(path.join(require("os").tmpdir(), "apc-trace-sources-"));
    t.after(() => fs.rmSync(tempDir, {recursive: true, force: true}));
    const mainSource = path.join(tempDir, "main.ct");
    const importedSource = path.join(tempDir, "imported.ct");
    const missingSameBasename = path.join(tempDir, "missing", "main.ct");
    fs.writeFileSync(mainSource, "MAIN FILE\nmain breakpoint\n", "utf8");
    fs.writeFileSync(importedSource, "IMPORTED FILE\nimport breakpoint\n", "utf8");
    const tracePath = path.join(tempDir, "multi-source.json");
    fs.writeFileSync(tracePath, JSON.stringify({
        format: "apc-stack-trace",
        source: {file: mainSource, lines: ["MAIN EMBEDDED", "main breakpoint"]},
        steps: [
            {step: 0, pc: 0, opcode: "OP_0", sourceFile: mainSource, sourceLine: 1},
            {step: 1, pc: 1, opcode: "OP_1", sourceFile: mainSource, sourceLine: 2},
            {step: 2, pc: 2, opcode: "OP_2", source: {file: importedSource, line: 2}},
            {step: 3, pc: 3, opcode: "OP_3", sourceFile: missingSameBasename, sourceLine: 1}
        ]
    }), "utf8");
    const client = attach(createAdapter());
    t.after(() => client.dispose());
    assertSuccess(assert, await client.request("launch", {tracePath}));

    assertSuccess(assert, await client.request("setBreakpoints", {
        source: {path: mainSource},
        breakpoints: [{line: 2}]
    }));
    assertSuccess(assert, await client.request("setBreakpoints", {
        source: {path: importedSource},
        breakpoints: [{line: 2}]
    }));
    assertSuccess(assert, await client.request("continue"));
    assert.strictEqual(
        assertSuccess(assert, await client.request("evaluate", {expression: "pc"})).result,
        "1"
    );
    assertSuccess(assert, await client.request("continue"));
    assert.strictEqual(
        assertSuccess(assert, await client.request("evaluate", {expression: "pc"})).result,
        "2"
    );

    const imported = assertSuccess(assert, await client.request("source", {
        source: {path: importedSource}
    }));
    assert.strictEqual(imported.content, "IMPORTED FILE\nimport breakpoint\n");
    assert(!imported.content.includes("MAIN EMBEDDED"));
    const missingImport = assertSuccess(assert, await client.request("source", {
        source: {path: missingSameBasename}
    }));
    assert.strictEqual(missingImport.content, "");
});

test("Trace DAP launch 严格校验字段路径且空 Trace 不制造栈帧", async (t) => {
    const tempDir = fs.mkdtempSync(path.join(require("os").tmpdir(), "apc-trace-schema-"));
    t.after(() => fs.rmSync(tempDir, {recursive: true, force: true}));
    const invalidPath = path.join(tempDir, "invalid.json");
    fs.writeFileSync(invalidPath, JSON.stringify({
        format: "apc-stack-trace",
        steps: [{step: 0, pc: "bad"}]
    }), "utf8");
    const invalidClient = attach(createAdapter());
    t.after(() => invalidClient.dispose());
    const invalid = await invalidClient.request("launch", {tracePath: invalidPath});
    assert.strictEqual(invalid.success, false);
    assert.match(invalid.message, /\$\.steps\[0\]\.pc|\/steps\/0\/pc/);
    assert.match(invalid.message, /integer/);
    assert.strictEqual(invalidClient.events("initialized").length, 0);

    const emptyPath = path.join(tempDir, "empty.json");
    fs.writeFileSync(emptyPath, JSON.stringify({
        format: "apc-stack-trace",
        steps: []
    }), "utf8");
    const emptyClient = attach(createAdapter());
    t.after(() => emptyClient.dispose());
    assertSuccess(assert, await emptyClient.request("launch", {tracePath: emptyPath}));
    const stack = assertSuccess(assert, await emptyClient.request("stackTrace"));
    assert.deepStrictEqual(stack.stackFrames, []);
    assert.strictEqual(stack.totalFrames, 0);
});
