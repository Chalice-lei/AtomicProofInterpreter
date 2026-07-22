"use strict";

const assert = require("assert");
const path = require("path");
const {after, before, test} = require("node:test");
const {closeServer, launchChromium, startServer} = require("../helpers/browser");
const {largeTrace} = require("../helpers/trace_fixtures");

const extensionRoot = path.resolve(__dirname, "..", "..");
const repoRoot = path.resolve(extensionRoot, "..", "..");
let browser;
let page;
let server;

before(async () => {
    server = await startServer(repoRoot);
    browser = await launchChromium();
    page = await browser.newPage({viewport: {width: 1440, height: 1000}});
    await page.goto(
        `http://127.0.0.1:${server.address().port}/tools/vscode_stack_visualizer/stack_visualizer/index.html`,
        {waitUntil: "networkidle"}
    );
});

after(async () => {
    await page?.close();
    await browser?.close();
    await closeServer(server);
});

test("10,000 step 与 1,000 元素栈保持虚拟化和可交互性能", {timeout: 30000}, async () => {
    const trace = largeTrace(10000, 1000);
    const started = Date.now();
    await page.evaluate((value) => loadTraceJson(value), trace);
    const elapsed = Date.now() - started;
    assert(elapsed < 8000, `large trace initial render took ${elapsed}ms`);
    assert.strictEqual(await page.locator("#stepCount").textContent(), "1 / 10000");
    assert((await page.locator("#mainStackList").textContent()).includes("of 1000"));
    assert(await page.locator("#mainStackList .stack-card").count() < 100);
    assert(await page.locator("#eventRail .event-dot").count() < 20);

    await page.locator("#mainStackList").evaluate((element) => {
        element.scrollTop = element.scrollHeight;
        element.dispatchEvent(new Event("scroll"));
    });
    await page.waitForTimeout(100);
    const deepText = await page.locator("#mainStackList").textContent();
    assert(deepText.includes("large-0") || deepText.includes("0x0000"));

    const navigationMs = await page.evaluate(() => {
        const start = performance.now();
        for (let index = 0; index < 250; index++) {
            setStep((index * 37) % steps().length);
        }
        return performance.now() - start;
    });
    assert(navigationMs < 6000, `250 large-trace renders took ${navigationMs.toFixed(1)}ms`);

    const memory = await page.evaluate(async () => {
        if (!performance.memory) return null;
        const before = performance.memory.usedJSHeapSize;
        for (let index = 0; index < 100; index++) {
            setStep(index % 2 ? 0 : steps().length - 1);
        }
        await new Promise((resolve) => setTimeout(resolve, 50));
        return performance.memory.usedJSHeapSize - before;
    });
    assert.notStrictEqual(memory, null, "Chromium performance.memory is required for heap regression checks");
    assert(memory < 128 * 1024 * 1024, `heap grew by ${(memory / 1024 / 1024).toFixed(1)} MiB`);
});

test("连续播放能自动抵达末尾并清理 timer", {timeout: 10000}, async () => {
    await page.evaluate((value) => loadTraceJson(value), largeTrace(12, 0));
    await page.locator("#speed").evaluate((element) => {
        element.value = "4";
        element.dispatchEvent(new Event("input", {bubbles: true}));
    });
    await page.locator("#playBtn").click();
    await page.waitForFunction(() =>
        document.querySelector("#stepCount").textContent === "12 / 12" &&
        document.querySelector("#playBtn").textContent === "Play",
    null, {timeout: 8000});
    assert.strictEqual(await page.evaluate(() => state.timer), null);
});
