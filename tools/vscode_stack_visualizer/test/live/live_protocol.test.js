"use strict";

const assert = require("assert");
const cp = require("child_process");
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
const repoRoot = path.resolve(extensionRoot, "..", "..");
const interpreter = process.env.APC_COMPILER || path.join(
    repoRoot,
    "build",
    "bin",
    process.platform === "win32" ? "utxo_Interpreter.exe" : "utxo_Interpreter"
);
const contract = path.join(
    repoRoot,
    "test",
    "debugger_regression",
    "debug_stack_visualizer_alt.ct"
);
const asyncPauseContract = path.join(
    repoRoot,
    "test",
    "debugger_regression",
    "debug_async_pause.ct"
);
const runtimeErrorContract = path.join(
    repoRoot,
    "test",
    "debugger_regression",
    "debug_runtime_error.ct"
);
const privateFunctionContract = path.join(
    repoRoot,
    "test",
    "debugger_regression",
    "debug_private_function_step.ct"
);

async function waitForCondition(predicate, message, timeoutMs = 3000)
{
    const deadline = Date.now() + timeoutMs;
    while (Date.now() < deadline) {
        if (predicate()) return;
        await new Promise((resolve) => setTimeout(resolve, 10));
    }
    throw new Error(message);
}

class JsonlClient
{
    constructor(args, timeoutMs = 15000)
    {
        if (!fs.existsSync(interpreter)) {
            throw new Error(`Core live test requires interpreter: ${interpreter}`);
        }
        this.child = cp.spawn(interpreter, args, {
            cwd: repoRoot,
            stdio: ["pipe", "pipe", "pipe"]
        });
        this.buffer = "";
        this.stderr = "";
        this.messages = [];
        this.history = [];
        this.waiters = new Set();
        this.seq = 1;
        this.closed = false;
        this.closeInfo = null;
        this.terminatedSeen = false;
        this.child.stdout.on("data", (chunk) => this.onData(chunk));
        this.child.stderr.on("data", (chunk) => { this.stderr += chunk; });
        this.child.on("close", (code, signal) => {
            this.closed = true;
            this.closeInfo = {code, signal};
            this.flushWaiters();
        });
        this.timer = setTimeout(() => {
            this.child.kill("SIGTERM");
            setTimeout(() => this.child.kill("SIGKILL"), 1000).unref();
        }, timeoutMs);
    }

    onData(chunk)
    {
        this.buffer += chunk.toString();
        let newline;
        while ((newline = this.buffer.indexOf("\n")) >= 0) {
            const line = this.buffer.slice(0, newline).trim();
            this.buffer = this.buffer.slice(newline + 1);
            if (line) {
                const message = JSON.parse(line);
                if (message.type === "event" && message.event === "terminated") {
                    this.terminatedSeen = true;
                }
                this.messages.push(message);
                this.history.push(message);
                this.flushWaiters();
            }
        }
    }

    flushWaiters()
    {
        for (const waiter of [...this.waiters]) {
            const index = this.messages.findIndex(waiter.predicate);
            if (index >= 0) {
                this.waiters.delete(waiter);
                clearTimeout(waiter.timer);
                waiter.resolve(this.messages.splice(index, 1)[0]);
            } else if (this.closed) {
                this.waiters.delete(waiter);
                clearTimeout(waiter.timer);
                waiter.reject(new Error(
                    `debug-server exited before expected message: ${JSON.stringify(this.closeInfo)}\n${this.stderr}`
                ));
            }
        }
    }

    waitFor(predicate, label, timeoutMs = 5000)
    {
        const index = this.messages.findIndex(predicate);
        if (index >= 0) {
            return Promise.resolve(this.messages.splice(index, 1)[0]);
        }
        if (this.closed) {
            return Promise.reject(new Error(`debug-server already exited while waiting for ${label}`));
        }
        return new Promise((resolve, reject) => {
            const waiter = {predicate, resolve, reject, timer: null};
            waiter.timer = setTimeout(() => {
                this.waiters.delete(waiter);
                reject(new Error(
                    `Timed out waiting for ${label}; messages=${JSON.stringify(this.messages)} stderr=${this.stderr}`
                ));
            }, timeoutMs);
            this.waiters.add(waiter);
        });
    }

    event(name)
    {
        return this.waitFor(
            (message) => message.type === "event" && message.event === name,
            `event ${name}`
        );
    }

    async request(command, body = {})
    {
        const seq = this.seq++;
        this.child.stdin.write(JSON.stringify({seq, command, ...body}) + "\n");
        return this.waitFor(
            (message) => message.type === "response" && message.request_seq === seq,
            `response ${command}`
        );
    }

    async disconnect()
    {
        if (!this.closed && this.child.stdin.writable) {
            const response = await this.request("disconnect");
            assert.strictEqual(response.success, true, response.message);
            if (!this.terminatedSeen) {
                await this.event("terminated");
            }
            this.child.stdin.end();
        }
        await this.waitForExit();
    }

    waitForExit(timeoutMs = 3000)
    {
        if (this.closed) return Promise.resolve(this.closeInfo);
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => reject(new Error("debug-server did not exit")), timeoutMs);
            this.child.once("close", (code, signal) => {
                clearTimeout(timer);
                resolve({code, signal});
            });
        });
    }

    async cleanup()
    {
        clearTimeout(this.timer);
        if (!this.closed) {
            this.child.kill("SIGTERM");
            try {
                await this.waitForExit(1000);
            } catch (_error) {
                this.child.kill("SIGKILL");
                await this.waitForExit(1000).catch(() => {});
            }
        }
    }
}

function serverArgs(...extra)
{
    return ["debug-server", contract, "test_alt_roundtrip", "5", ...extra];
}

test("真实 JSONL debug-server 覆盖初始化、步进、变量、evaluate 和断开", async (t) => {
    const client = new JsonlClient(serverArgs());
    t.after(() => client.cleanup());
    const ready = await client.event("ready");
    assert.strictEqual(ready.body.snapshot.functionName, "test_alt_roundtrip");

    let response = await client.request("initialize");
    assert.strictEqual(response.success, true, response.message);
    assert.strictEqual(response.body.supportsBreakpoints, true);

    for (const scope of ["instruction", "mainStack", "altStack", "callStack", "warnings"]) {
        response = await client.request("variables", {scope});
        assert.strictEqual(response.success, true, `${scope}: ${response.message}`);
        assert(Array.isArray(response.body.variables));
    }
    for (const expression of ["pc", "opcode", "line", "main.length", "alt.length", "json"]) {
        response = await client.request("evaluate", {expression});
        assert.strictEqual(response.success, true, `${expression}: ${response.message}`);
        assert(Object.prototype.hasOwnProperty.call(response.body, "result"));
    }

    for (const command of ["stepIn", "next", "stepIn", "stepOut"]) {
        response = await client.request(command);
        assert.strictEqual(response.success, true, `${command}: ${response.message}`);
        const event = await client.waitFor(
            (message) => message.type === "event" &&
                (message.event === "stopped" || message.event === "terminated"),
            `${command} execution event`
        );
        assert(["stopped", "terminated"].includes(event.event));
    }
    await client.disconnect();
    assert.strictEqual(client.closeInfo.code, 0, client.stderr);
});

test("真实 JSONL breakpoint + continue 在源码行停止", async (t) => {
    const client = new JsonlClient(serverArgs());
    t.after(() => client.cleanup());
    await client.event("ready");
    const wrongSource = await client.request("setBreakpoints", {
        source: {path: path.join(path.dirname(contract), "not-the-contract.ct")},
        breakpoints: [{line: 7}]
    });
    assert.strictEqual(wrongSource.success, true, wrongSource.message);
    assert.strictEqual(wrongSource.body.breakpoints[0].verified, false);

    const setResponse = await client.request("setBreakpoints", {
        source: {path: contract},
        breakpoints: [{line: 7}, {line: 9999}]
    });
    assert.strictEqual(setResponse.success, true, setResponse.message);
    assert.strictEqual(setResponse.body.breakpoints[0].verified, true);
    assert.strictEqual(setResponse.body.breakpoints[1].verified, false);

    const continued = await client.request("continue");
    assert.strictEqual(continued.success, true, continued.message);
    const stopped = await client.event("stopped");
    assert.strictEqual(stopped.body.reason, "breakpoint");
    assert.strictEqual(stopped.body.snapshot.source.line, 7);
    await client.disconnect();
});

test("真实 JSONL 运行时错误作为 exception 停止并携带错误详情", async (t) => {
    const client = new JsonlClient([
        "debug-server",
        runtimeErrorContract,
        "test_runtime_error"
    ]);
    t.after(() => client.cleanup());
    await client.event("ready");

    const response = await client.request("continue");
    assert.strictEqual(response.success, true, response.message);
    await client.event("continued");
    const stopped = await client.event("stopped");
    assert.strictEqual(stopped.body.reason, "exception");
    assert.match(stopped.body.description, /OP_VERIFY failed/);
    assert.match(stopped.body.snapshot.error, /OP_VERIFY failed/);
    assert.strictEqual(client.terminatedSeen, false);

    const duplicateStopsBefore = client.messages.filter((message) =>
        message.type === "event" && message.event === "stopped"
    ).length;
    const staleFrame = await client.request("variables", {
        scope: "locals",
        frameId: 999999
    });
    assert.strictEqual(staleFrame.success, false);
    const duplicateStopsAfter = client.messages.filter((message) =>
        message.type === "event" && message.event === "stopped"
    ).length;
    assert.strictEqual(duplicateStopsAfter, duplicateStopsBefore);
    await client.disconnect();
});

test("真实 JSONL 返回完整调用栈、帧变量并拒绝未知表达式", async (t) => {
    const client = new JsonlClient([
        "debug-server",
        privateFunctionContract,
        "test_private_step",
        "4",
        "3"
    ]);
    t.after(() => client.cleanup());
    await client.event("ready");

    for (let index = 0; index < 2; index += 1) {
        const step = await client.request("stepIn");
        assert.strictEqual(step.success, true, step.message);
        await client.event("stopped");
    }

    const stackTrace = await client.request("stackTrace");
    assert.strictEqual(stackTrace.success, true, stackTrace.message);
    assert.deepStrictEqual(
        stackTrace.body.frames.map((frame) => frame.name),
        ["_sum_pair", "test_private_step"]
    );
    assert(stackTrace.body.frames.every((frame) => Number(frame.id) > 0));
    assert(stackTrace.body.frames.every((frame) => frame.source.line > 0));

    const calleeFrame = stackTrace.body.frames[0];
    const locals = await client.request("variables", {
        scope: "locals",
        frameId: calleeFrame.id
    });
    assert.strictEqual(locals.success, true, locals.message);
    const localMap = Object.fromEntries(
        locals.body.variables.map((item) => [item.name, item])
    );
    assert.strictEqual(localMap.x.value, "5");
    assert.strictEqual(localMap.y.value, "3");

    const evaluated = await client.request("evaluate", {
        expression: "x",
        frameId: calleeFrame.id
    });
    assert.strictEqual(evaluated.success, true, evaluated.message);
    assert.strictEqual(evaluated.body.result, "5");

    const callerFrame = stackTrace.body.frames[1];
    const callerLocals = await client.request("variables", {
        scope: "locals",
        frameId: callerFrame.id
    });
    assert.strictEqual(callerLocals.success, true, callerLocals.message);
    assert.deepStrictEqual(
        callerLocals.body.variables.map((item) => [item.name, item.value]),
        [["before", "5"]]
    );
    const callerEvaluation = await client.request("evaluate", {
        expression: "before",
        frameId: callerFrame.id
    });
    assert.strictEqual(callerEvaluation.success, true, callerEvaluation.message);
    assert.strictEqual(callerEvaluation.body.result, "5");
    for (const [expression, expected] of [
        ["pc", String(callerFrame.pc)],
        ["functionName", "test_private_step"],
        ["main[0].int", "5"]
    ]) {
        const frameEvaluation = await client.request("evaluate", {
            expression,
            frameId: callerFrame.id
        });
        assert.strictEqual(
            frameEvaluation.success,
            true,
            frameEvaluation.message
        );
        assert.strictEqual(frameEvaluation.body.result, expected);
    }
    const callerMainStack = await client.request("variables", {
        scope: "mainStack",
        frameId: callerFrame.id
    });
    assert.strictEqual(callerMainStack.success, true, callerMainStack.message);
    assert.match(callerMainStack.body.variables[0].value, /int=5/);

    const stoppedBefore = client.messages.filter((message) =>
        message.type === "event" && message.event === "stopped"
    ).length;
    const unknown = await client.request("evaluate", {
        expression: "does_not_exist",
        frameId: calleeFrame.id
    });
    assert.strictEqual(unknown.success, false);
    assert.match(unknown.message, /not found|unknown|failed|未找到/i);
    const stoppedAfter = client.messages.filter((message) =>
        message.type === "event" && message.event === "stopped"
    ).length;
    assert.strictEqual(stoppedAfter, stoppedBefore);
    await client.disconnect();
});

test("真实 JSONL 非零函数入口在 ready 阶段返回匹配的顶层帧", async (t) => {
    const client = new JsonlClient([
        "debug-server",
        privateFunctionContract,
        "_sum_pair",
        "4",
        "3"
    ]);
    t.after(() => client.cleanup());

    const ready = await client.event("ready");
    assert.strictEqual(ready.body.snapshot.functionName, "_sum_pair");
    const stackTrace = await client.request("stackTrace");
    assert.strictEqual(stackTrace.success, true, stackTrace.message);
    assert.deepStrictEqual(
        stackTrace.body.frames.map((frame) => frame.name),
        ["_sum_pair"]
    );
    assert.strictEqual(
        stackTrace.body.frames[0].pc,
        ready.body.snapshot.pc
    );
    await client.disconnect();
});

test("真实 JSONL 顶层 stepOut 直接终止且不产生伪停止位置", async (t) => {
    const client = new JsonlClient(serverArgs());
    t.after(() => client.cleanup());
    await client.event("ready");

    const response = await client.request("stepOut");
    assert.strictEqual(response.success, true, response.message);
    assert.strictEqual(response.body.snapshot.state, "finished");
    await client.event("terminated");
    assert.strictEqual(
        client.messages.some((message) =>
            message.type === "event" && message.event === "stopped"
        ),
        false
    );
    await client.disconnect();
});

test("真实 JSONL continue 可被并发 pause 中断且不会先终止", async (t) => {
    const client = new JsonlClient([
        "debug-server",
        asyncPauseContract,
        "test_async_pause"
    ], 20000);
    t.after(() => client.cleanup());
    await client.event("ready");

    const continueSeq = client.seq++;
    const inspectSeq = client.seq++;
    const initializeSeq = client.seq++;
    const pauseSeq = client.seq++;
    client.child.stdin.write(
        JSON.stringify({seq: continueSeq, command: "continue"}) + "\n" +
        JSON.stringify({seq: inspectSeq, command: "snapshot"}) + "\n" +
        JSON.stringify({seq: initializeSeq, command: "initialize"}) + "\n" +
        JSON.stringify({seq: pauseSeq, command: "pause"}) + "\n"
    );

    const continueResponse = await client.waitFor(
        (message) => message.type === "response" &&
            message.request_seq === continueSeq,
        "continue response"
    );
    assert.strictEqual(continueResponse.success, true, continueResponse.message);
    assert.strictEqual(continueResponse.body.allThreadsContinued, true);

    const continued = await client.event("continued");
    assert.strictEqual(continued.body.allThreadsContinued, true);
    const inspectResponse = await client.waitFor(
        (message) => message.type === "response" &&
            message.request_seq === inspectSeq,
        "running snapshot response"
    );
    assert.strictEqual(inspectResponse.success, false);
    assert.match(inspectResponse.message, /program is running/);
    const initializeResponse = await client.waitFor(
        (message) => message.type === "response" &&
            message.request_seq === initializeSeq,
        "running initialize response"
    );
    assert.strictEqual(initializeResponse.success, false);
    assert.match(initializeResponse.message, /program is running/);
    const pauseResponse = await client.waitFor(
        (message) => message.type === "response" &&
            message.request_seq === pauseSeq,
        "pause response"
    );
    assert.strictEqual(pauseResponse.success, true, pauseResponse.message);

    const stopped = await client.event("stopped");
    assert.strictEqual(stopped.body.reason, "pause");
    assert.strictEqual(stopped.body.snapshot.state, "paused");
    assert(
        client.history.indexOf(pauseResponse) < client.history.indexOf(stopped),
        "pause response must precede the asynchronous stopped event"
    );
    assert.strictEqual(client.terminatedSeen, false);

    const resumed = await client.request("continue");
    assert.strictEqual(resumed.success, true, resumed.message);
    await client.event("continued");
    await client.event("terminated");
    assert.strictEqual(client.terminatedSeen, true);
    await client.disconnect();
});

test("真实 Live DAP Adapter 连接真实解释器并支持 terminate", async (t) => {
    if (!fs.existsSync(interpreter)) {
        throw new Error(`Core live test requires interpreter: ${interpreter}`);
    }
    const vscode = createVscodeMock({workspaceRoot: repoRoot});
    const extension = loadExtensionWithMock(path.join(extensionRoot, "extension.js"), vscode);
    const adapter = new extension.__test.AtomicProofLiveDebugAdapter();
    const client = attach(adapter);
    t.after(() => client.dispose());

    assertSuccess(assert, await client.request("launch", {
        contractPath: contract,
        functionName: "test_alt_roundtrip",
        arguments: ["5"],
        interpreterPath: interpreter
    }, 10000));
    const breakpoints = assertSuccess(assert, await client.request("setBreakpoints", {
        source: {path: contract},
        breakpoints: [{line: 7}]
    })).breakpoints;
    assert.strictEqual(breakpoints[0].verified, true);
    assertSuccess(assert, await client.request("stepIn"));
    assertSuccess(assert, await client.request("next"));
    const scopes = assertSuccess(assert, await client.request("scopes", {frameId: 1})).scopes;
    assert(scopes.some((scope) => scope.name === "Main Stack"));
    assert.strictEqual(
        assertSuccess(assert, await client.request("evaluate", {expression: "pc"})).result,
        "2"
    );
    assertSuccess(assert, await client.request("terminate"));
    assert.strictEqual(adapter.child, null);
    assert.strictEqual(client.events("terminated").length, 1);
});

test("真实 Live DAP 将运行时错误映射为 exception、Errors 和 exceptionInfo", async (t) => {
    const vscode = createVscodeMock({workspaceRoot: repoRoot});
    const extension = loadExtensionWithMock(path.join(extensionRoot, "extension.js"), vscode);
    const adapter = new extension.__test.AtomicProofLiveDebugAdapter();
    const client = attach(adapter);
    t.after(() => client.dispose());

    assertSuccess(assert, await client.request("launch", {
        contractPath: runtimeErrorContract,
        functionName: "test_runtime_error",
        interpreterPath: interpreter
    }, 10000));
    assertSuccess(assert, await client.request("continue"));
    await waitForCondition(
        () => client.events("stopped").some((event) =>
            event.body.reason === "exception"
        ),
        "DAP did not emit an exception stop"
    );

    const stopped = client.events("stopped").at(-1);
    assert.match(stopped.body.description, /OP_VERIFY failed/);
    const exception = assertSuccess(
        assert,
        await client.request("exceptionInfo", {threadId: 1})
    );
    assert.match(exception.description, /OP_VERIFY failed/);

    const frame = assertSuccess(
        assert,
        await client.request("stackTrace", {threadId: 1})
    ).stackFrames[0];
    const scopes = assertSuccess(
        assert,
        await client.request("scopes", {frameId: frame.id})
    ).scopes;
    const errors = scopes.find((scope) => scope.name === "Errors");
    const errorVariables = assertSuccess(
        assert,
        await client.request("variables", {
            variablesReference: errors.variablesReference
        })
    ).variables;
    assert.match(errorVariables[0].value, /OP_VERIFY failed/);
    assert(client.events("output").some((event) =>
        event.body.category === "stderr" &&
        /OP_VERIFY failed/.test(event.body.output)
    ));
    const invalidContinue = await client.request("continue");
    assert.strictEqual(invalidContinue.success, false);
    assert.match(invalidContinue.message, /runtime error.*restart/i);
    assertSuccess(assert, await client.request("disconnect"));
});

test("编译错误、函数不存在、参数错误和 txFile 错误均失败且退出", async (t) => {
    const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "apc-live-errors-"));
    t.after(() => fs.rmSync(tempDir, {recursive: true, force: true}));
    const invalidContract = path.join(tempDir, "invalid.ct");
    fs.writeFileSync(invalidContract, "Contract Broken:\n    def bad(\n", "utf8");

    const cases = [
        ["compile error", ["debug-server", invalidContract]],
        ["missing function", ["debug-server", contract, "does_not_exist"]],
        ["argument error", ["debug-server", contract, "test_alt_roundtrip", "not-an-int"]],
        ["too many args", ["debug-server", contract, "test_alt_roundtrip", "1", "2"]],
        ["txFile error", [
            "debug-server",
            contract,
            "test_alt_roundtrip",
            "5",
            "--txfile",
            path.join(tempDir, "missing-tx.json")
        ]]
    ];

    for (const [label, args] of cases) {
        const client = new JsonlClient(args, 10000);
        const error = await client.event("error");
        assert(error.body.message, `${label}: empty error message`);
        await client.event("terminated");
        await client.waitForExit();
        clearTimeout(client.timer);
        assert.notStrictEqual(client.closeInfo.code, 0, label);
        await client.cleanup();
    }
});
