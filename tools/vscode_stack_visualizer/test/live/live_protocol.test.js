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
        this.waiters = new Set();
        this.seq = 1;
        this.closed = false;
        this.closeInfo = null;
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
                this.messages.push(JSON.parse(line));
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
            await this.event("terminated");
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
