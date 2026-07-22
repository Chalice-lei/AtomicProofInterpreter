"use strict";

const assert = require("assert");
const fs = require("fs");
const os = require("os");
const path = require("path");
const test = require("node:test");
const {assertSuccess, attach} = require("../helpers/dap_client");
const {
    createVscodeMock,
    loadExtensionWithMock
} = require("../helpers/vscode_mock");

const extensionRoot = path.resolve(__dirname, "..", "..");
const extensionPath = path.join(extensionRoot, "extension.js");
const fakeInterpreter = path.join(
    extensionRoot,
    "test",
    "fixtures",
    "fake_live_interpreter.js"
);

function createAdapter(workspaceRoot)
{
    const vscode = createVscodeMock({workspaceRoot});
    const extension = loadExtensionWithMock(extensionPath, vscode);
    return new extension.__test.AtomicProofLiveDebugAdapter();
}

function createContract(t, basename = "live.ct")
{
    const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "apc-live-dap-"));
    const contract = path.join(tempDir, basename);
    fs.writeFileSync(contract, "Contract Live:\n    def main():\n        Return(1)\n", "utf8");
    fs.writeFileSync(
        path.join(tempDir, "debug-server"),
        `process.argv.splice(2, 0, "debug-server"); require(${JSON.stringify(fakeInterpreter)});\n`,
        "utf8"
    );
    t.after(() => fs.rmSync(tempDir, {recursive: true, force: true}));
    return contract;
}

async function waitFor(predicate, message, timeoutMs = 2000)
{
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        if (predicate()) {
            return;
        }
        await new Promise((resolve) => setTimeout(resolve, 20));
    }
    throw new Error(message);
}

test("Live DAP 完整覆盖 launch、断点、continue/next/stepIn/stepOut", async (t) => {
    const contract = createContract(t);
    const client = attach(createAdapter(path.dirname(contract)));
    t.after(() => client.dispose());

    const capabilities = assertSuccess(assert, await client.request("initialize"));
    assert.strictEqual(capabilities.supportsStepBack, false);
    assert.strictEqual(capabilities.supportsTerminateRequest, true);
    assertSuccess(assert, await client.request("launch", {
        contractPath: contract,
        functionName: "main",
        arguments: ["5"],
        interpreterPath: process.execPath
    }));
    assert.strictEqual(client.events("initialized").length, 1);

    const breakpoints = assertSuccess(assert, await client.request("setBreakpoints", {
        source: {path: contract},
        breakpoints: [{line: 2}, {line: 99}]
    })).breakpoints;
    assert.strictEqual(breakpoints[0].verified, true);
    assert.strictEqual(breakpoints[1].verified, false);

    assertSuccess(assert, await client.request("configurationDone"));
    let stoppedCount = client.events("stopped").length;
    for (const command of ["next", "stepIn", "stepOut", "continue"]) {
        assertSuccess(assert, await client.request(command));
        await waitFor(
            () => client.events("stopped").length > stoppedCount,
            `${command} did not emit a stopped event`
        );
        stoppedCount = client.events("stopped").length;
        assert.strictEqual(client.events("stopped").at(-1).body.reason, "step");
    }
});

test("Live DAP variables/evaluate 覆盖 instruction、双栈、调用栈和 warnings", async (t) => {
    const contract = createContract(t);
    const client = attach(createAdapter(path.dirname(contract)));
    t.after(() => client.dispose());
    assertSuccess(assert, await client.request("launch", {
        contractPath: contract,
        functionName: "main",
        interpreterPath: process.execPath
    }));
    assertSuccess(assert, await client.request("stepIn"));
    assertSuccess(assert, await client.request("stepIn"));

    const frame = assertSuccess(assert, await client.request("stackTrace")).stackFrames[0];
    assert.strictEqual(frame.name, "fake_main");
    assert.strictEqual(frame.line, 3);

    const scopes = assertSuccess(assert, await client.request("scopes", {frameId: 1})).scopes;
    assert.deepStrictEqual(
        scopes.map((scope) => scope.name),
        ["Instruction", "Main Stack", "Alt Stack", "Call Stack", "Warnings"]
    );
    const variablesByScope = new Map();
    for (const scope of scopes) {
        variablesByScope.set(scope.name, assertSuccess(assert, await client.request("variables", {
            variablesReference: scope.variablesReference
        })).variables);
    }
    assert(variablesByScope.get("Instruction").some((item) => item.name === "currentOpcode"));
    assert.match(variablesByScope.get("Main Stack")[0].value, /int=12/);
    assert.match(variablesByScope.get("Alt Stack")[0].value, /int=99/);
    assert.match(variablesByScope.get("Call Stack")[0].value, /returnPC=6/);
    assert.strictEqual(variablesByScope.get("Warnings")[0].value, "fake warning");

    assert.strictEqual(
        assertSuccess(assert, await client.request("evaluate", {expression: "pc"})).result,
        "2"
    );
    assert.strictEqual(
        assertSuccess(assert, await client.request("evaluate", {expression: "alt.length"})).result,
        "1"
    );
    const json = assertSuccess(assert, await client.request("evaluate", {expression: "json"}));
    assert(json.variablesReference > 0);
    const children = assertSuccess(assert, await client.request("variables", {
        variablesReference: json.variablesReference
    })).variables;
    assert(children.some((item) => item.name === "pc"));
});

test("Live DAP disconnect 与 terminate 都结束子进程且发送 terminated", async (t) => {
    const contract = createContract(t);

    const disconnectAdapter = createAdapter(path.dirname(contract));
    const disconnected = attach(disconnectAdapter);
    t.after(() => disconnected.dispose());
    assertSuccess(assert, await disconnected.request("launch", {
        contractPath: contract,
        functionName: "main",
        interpreterPath: process.execPath
    }));
    assertSuccess(assert, await disconnected.request("disconnect"));
    await waitFor(
        () => disconnected.events("terminated").length > 0,
        "disconnect did not emit terminated"
    );

    const terminateAdapter = createAdapter(path.dirname(contract));
    const terminated = attach(terminateAdapter);
    t.after(() => terminated.dispose());
    assertSuccess(assert, await terminated.request("launch", {
        contractPath: contract,
        functionName: "main",
        interpreterPath: process.execPath
    }));
    assert(terminateAdapter.child);
    assertSuccess(assert, await terminated.request("terminate"));
    assert.strictEqual(terminateAdapter.child, null);
    assert.strictEqual(terminated.events("terminated").length, 1);
});

test("Live DAP 对缺配置、参数错误、txFile 和异常退出提供失败证据", async (t) => {
    const contract = createContract(t);
    const client = attach(createAdapter(path.dirname(contract)));
    t.after(() => client.dispose());

    let response = await client.request("launch", {});
    assert.strictEqual(response.success, false);
    assert.match(response.message, /missing contractPath/);

    response = await client.request("launch", {
        contractPath: contract,
        arguments: ["5"],
        interpreterPath: process.execPath
    });
    assert.strictEqual(response.success, false);
    assert.match(response.message, /functionName is required/);

    response = await client.request("launch", {
        contractPath: contract,
        functionName: "main",
        interpreterPath: path.join(path.dirname(contract), "missing-interpreter")
    });
    assert.strictEqual(response.success, false);
    assert.match(response.message, /Executable not found/);

    const txAdapter = createAdapter(path.dirname(contract));
    const txClient = attach(txAdapter);
    t.after(() => txClient.dispose());
    assertSuccess(assert, await txClient.request("launch", {
        contractPath: contract,
        functionName: "main",
        txFile: path.join(path.dirname(contract), "missing.json"),
        interpreterPath: process.execPath
    }));
    assertSuccess(assert, await txClient.request("disconnect"));

    const crashContract = createContract(t, "crash.ct");
    const crashClient = attach(createAdapter(path.dirname(crashContract)));
    t.after(() => crashClient.dispose());
    assertSuccess(assert, await crashClient.request("launch", {
        contractPath: crashContract,
        functionName: "main",
        interpreterPath: process.execPath
    }));
    await waitFor(
        () => crashClient.events("terminated").length > 0,
        "abnormal child exit did not terminate DAP session"
    );
    const output = crashClient.events("output").map((event) => event.body.output).join("");
    assert.match(output, /code 17/);
});

test("Live DAP 参数始终作为 argv 传递，不经过 shell", async (t) => {
    const contract = createContract(t);
    const marker = path.join(path.dirname(contract), "injected-marker");
    const client = attach(createAdapter(path.dirname(contract)));
    t.after(() => client.dispose());
    assertSuccess(assert, await client.request("launch", {
        contractPath: contract,
        functionName: "main",
        arguments: [`;touch ${marker}`, "$(uname)", "带 空格"],
        interpreterPath: process.execPath
    }));
    assert.strictEqual(fs.existsSync(marker), false);
    assertSuccess(assert, await client.request("terminate"));
});
