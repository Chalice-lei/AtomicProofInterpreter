"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const {after, before, test} = require("node:test");
const {closeServer, launchChromium, startServer} = require("../helpers/browser");
const {edgeCaseTrace, largeTrace} = require("../helpers/trace_fixtures");
const {
    createVscodeMock,
    loadExtensionWithMock
} = require("../helpers/vscode_mock");

const extensionRoot = path.resolve(__dirname, "..", "..");
const repoRoot = path.resolve(extensionRoot, "..", "..");
const visualizerUrlPath = "/tools/vscode_stack_visualizer/stack_visualizer/index.html";
let browser;
let context;
let server;
let origin;

before(async () => {
    server = await startServer(repoRoot);
    origin = `http://127.0.0.1:${server.address().port}`;
    browser = await launchChromium();
    context = await browser.newContext({viewport: {width: 1440, height: 1000}});
    await context.grantPermissions(["clipboard-read", "clipboard-write"], {origin});
    await context.addInitScript(() => {
        window.__apcMessages = [];
        window.acquireVsCodeApi = () => ({
            postMessage: (message) => window.__apcMessages.push(message)
        });
    });
});

after(async () => {
    await context?.close();
    await browser?.close();
    await closeServer(server);
});

async function openExample(name)
{
    const page = await context.newPage();
    const pageErrors = [];
    page.on("pageerror", (error) => pageErrors.push(error));
    await page.goto(
        `${origin}${visualizerUrlPath}?trace=/examples/stack_traces/${name}`,
        {waitUntil: "networkidle"}
    );
    await page.locator(".step-summary-count").filter({hasText: "/"}).first().waitFor();
    return {page, pageErrors};
}

async function stepPosition(page)
{
    return page.locator("#stepCount").textContent();
}

async function buildRealWebviewHtml(trace)
{
    const vscode = createVscodeMock({workspaceRoot: repoRoot});
    const extension = loadExtensionWithMock(
        path.join(extensionRoot, "extension.js"),
        vscode
    );
    return extension.__test.buildWebviewHtml(
        {extensionPath: extensionRoot},
        {
            cspSource: "vscode-webview:",
            asWebviewUri: (uri) => String(uri.fsPath)
        },
        trace,
        path.join(repoRoot, "generated-stack-trace.json"),
        path.join(extensionRoot, "stack_visualizer", "index.html")
    );
}

test("离线 Webview 可加载全部 5 个现有 Trace", async () => {
    const examples = {
        "alt_roundtrip.json": "OP_10",
        "arithmetic_line_mapping.json": "OP_ROT",
        "branch_loop_false.json": "OP_0",
        "branch_loop_true.json": "OP_0",
        "push_builtin.json": "01"
    };
    for (const [name, opcode] of Object.entries(examples)) {
        const {page, pageErrors} = await openExample(name);
        assert((await page.locator("#stepSummary").textContent()).includes(opcode), name);
        assert.strictEqual(pageErrors.length, 0, `${name}: ${pageErrors.join("\n")}`);
        await page.close();
    }
});

test("Before/After/Diff 正确呈现 push、pop、move 和 reorder", async () => {
    const {page} = await openExample("alt_roundtrip.json");
    assert.strictEqual(await page.locator("#mainStackList .stack-card.pushed").count(), 1);
    await page.locator("#nextBtn").click();
    assert((await page.locator("#metricOpcode").textContent()).includes("OP_TOALTSTACK"));
    assert.strictEqual(await page.locator("#mainStackList .moved-out").count(), 1);
    assert.strictEqual(await page.locator("#altStackList .moved-in").count(), 1);

    await page.locator("#viewMode").selectOption("before");
    assert((await page.locator("#altStackList").textContent()).includes("No values"));
    await page.locator("#viewMode").selectOption("after");
    assert((await page.locator("#altStackList").textContent()).includes("0x0a"));
    await page.locator("#nextBtn").click();
    await page.locator("#viewMode").selectOption("diff");
    assert.strictEqual(await page.locator("#altStackList .moved-out").count(), 1);
    assert.strictEqual(await page.locator("#mainStackList .moved-in").count(), 1);
    await page.close();

    const arithmetic = await openExample("arithmetic_line_mapping.json");
    assert((await arithmetic.page.locator("#explainBody").textContent()).includes("order changed"));
    await arithmetic.page.locator("#timeline").evaluate((element) => {
        element.value = "2";
        element.dispatchEvent(new Event("input", {bubbles: true}));
    });
    assert((await arithmetic.page.locator("#mainStackList").textContent()).includes("popped"));
    assert((await arithmetic.page.locator("#mainStackList").textContent()).includes("pushed"));
    await arithmetic.page.close();
});

test("Prev/Next、播放、时间轴、搜索、过滤、事件、函数和源码导航可用", async () => {
    const {page} = await openExample("alt_roundtrip.json");
    await page.locator("#nextBtn").click();
    assert.strictEqual(await stepPosition(page), "2 / 5");
    await page.locator("#prevBtn").click();
    assert.strictEqual(await stepPosition(page), "1 / 5");
    await page.locator("#timeline").evaluate((element) => {
        element.value = "3";
        element.dispatchEvent(new Event("input", {bubbles: true}));
    });
    assert.strictEqual(await stepPosition(page), "4 / 5");

    await page.locator("#searchBox").fill("OP_RETURN");
    assert((await page.locator("#matchCount").textContent()).includes("1 matches"));
    await page.locator("#nextMatchBtn").click();
    assert((await page.locator("#metricOpcode").textContent()).includes("OP_RETURN"));
    await page.locator("#searchBox").fill("");
    await page.locator("#filterMode").selectOption("alt");
    await page.locator("#timeline").evaluate((element) => {
        element.value = "0";
        element.dispatchEvent(new Event("input", {bubbles: true}));
    });
    await page.locator("#nextBtn").click();
    assert((await page.locator("#metricOpcode").textContent()).includes("TOALT"));
    await page.locator("#nextEventBtn").click();
    assert((await page.locator("#metricOpcode").textContent()).includes("FROMALT"));

    assert.strictEqual(await page.locator("#functionSelect option").count(), 2);
    await page.locator("#functionSelect").selectOption("test_alt_roundtrip");
    assert.strictEqual(await stepPosition(page), "1 / 5");

    await page.locator("#openSourceBtn").click();
    const messages = await page.evaluate(() => window.__apcMessages);
    assert.deepStrictEqual(messages.at(-1), {
        type: "openSource",
        file: "test/debugger_regression/debug_stack_visualizer_alt.ct",
        line: 4,
        column: 21
    });

    await page.locator("#speed").evaluate((element) => {
        element.value = "4";
        element.dispatchEvent(new Event("input", {bubbles: true}));
    });
    await page.locator("#filterMode").selectOption("all");
    await page.locator("#playBtn").click();
    await page.waitForFunction(() => document.querySelector("#playBtn").textContent === "Play", null, {
        timeout: 3000
    });
    assert.strictEqual(await stepPosition(page), "5 / 5");
    await page.close();
});

test("书签在同一 Trace 持久化，并与其他 Trace 隔离", async () => {
    const first = await openExample("alt_roundtrip.json");
    await first.page.evaluate(() => {
        for (const key of Object.keys(localStorage)) {
            if (key.startsWith("apc.stackVisualizer.bookmarks:")) localStorage.removeItem(key);
        }
    });
    await first.page.reload({waitUntil: "networkidle"});
    await first.page.locator("#toggleBookmarkBtn").click();
    assert.strictEqual(await first.page.locator("#toggleBookmarkBtn").textContent(), "Unmark Step");
    await first.page.close();

    const same = await openExample("alt_roundtrip.json");
    assert.strictEqual(await same.page.locator("#toggleBookmarkBtn").textContent(), "Unmark Step");
    assert.strictEqual(await same.page.locator("#eventRail .bookmark, #eventRail .bookmarked").count(), 1);
    await same.page.close();

    const other = await openExample("arithmetic_line_mapping.json");
    assert.strictEqual(await other.page.locator("#toggleBookmarkBtn").textContent(), "Mark Step");
    await other.page.close();
});

test("elementId 的生产、移动、消费生命周期可点击和跳转", async () => {
    const {page} = await openExample("alt_roundtrip.json");
    await page.locator('[data-element-id="e2"]').click();
    const lifecycle = page.locator("#lifecyclePanel");
    assert((await lifecycle.textContent()).includes("Value Lifecycle: e2"));
    assert((await lifecycle.textContent()).includes("consumed at step 4"));
    assert.strictEqual(await lifecycle.locator(".lifecycle-event").count(), 4);
    await lifecycle.locator('.lifecycle-event[data-step="2"]').click();
    assert.strictEqual(await stepPosition(page), "3 / 5");
    assert.strictEqual(
        await page.locator('[data-element-id="e2"].selected').count(),
        2,
        "move diff should highlight both the outgoing and incoming representation"
    );
    await page.close();
});

test("Markdown/JSON 复制内容与诊断面板准确", async () => {
    const {page} = await openExample("alt_roundtrip.json");
    await page.locator("#copyMarkdownBtn").click();
    let clipboard = await page.evaluate(() => navigator.clipboard.readText());
    assert(clipboard.includes("# AtomicProof Trace Step 1"));
    assert(clipboard.includes("Main pushed: `1`"));

    await page.getByRole("tab", {name: "JSON"}).click();
    await page.locator("#copyJsonBtn").click();
    clipboard = await page.evaluate(() => navigator.clipboard.readText());
    assert(JSON.parse(clipboard).opcode === "OP_10");

    await page.getByRole("tab", {name: "Details"}).click();
    assert((await page.locator("#diagnosticsList").textContent()).includes("Trace metadata looks complete"));

    await page.evaluate(() => loadTraceJson({
        format: "legacy-stack-trace",
        source: {file: "missing.ct"},
        steps: [{step: 4, pc: 0}]
    }));
    const diagnostics = await page.locator("#diagnosticsList").textContent();
    assert(diagnostics.includes("not marked as apc-stack-trace"));
    assert(diagnostics.includes("do not include a source line"));
    assert(diagnostics.includes("do not include effects"));
    assert(diagnostics.includes("do not include opcode"));
    assert(diagnostics.includes("no lifecycle summary"));
    assert(diagnostics.includes("differ from their trace array position"));

    await page.evaluate(() => loadTraceJson({format: "apc-stack-trace", steps: []}));
    assert((await page.locator("#diagnosticsList").textContent()).includes("no execution steps"));
    await page.close();
});

test("恶意 HTML 只作为文本呈现，不执行 XSS", async () => {
    const {page, pageErrors} = await openExample("alt_roundtrip.json");
    const trace = edgeCaseTrace();
    trace.source.lines[0] = '<img src=x onerror="globalThis.__apcXss=1">';
    trace.steps[0].opcode = '</script><script>globalThis.__apcXss=2</script>';
    trace.steps[0].mainStackAfter[0].ascii = '<svg onload="globalThis.__apcXss=3">';
    await page.evaluate((value) => loadTraceJson(value), trace);
    assert.strictEqual(await page.evaluate(() => globalThis.__apcXss), undefined);
    assert((await page.locator("body").textContent()).includes("globalThis.__apcXss=2"));
    assert.strictEqual(await page.locator("img[src=x], svg[onload]").count(), 0);
    assert.strictEqual(pageErrors.length, 0);
    await page.close();
});

test("键盘操作、ARIA tab 状态和 axe 严重规则通过", async () => {
    const {page} = await openExample("alt_roundtrip.json");
    await page.keyboard.press("ArrowRight");
    assert.strictEqual(await stepPosition(page), "2 / 5");
    await page.keyboard.press("b");
    assert.strictEqual(await page.locator("#toggleBookmarkBtn").textContent(), "Unmark Step");
    await page.locator("#explainTabButton").focus();
    await page.keyboard.press("ArrowRight");
    assert.strictEqual(await page.locator("#detailsTabButton").getAttribute("aria-selected"), "true");
    assert.strictEqual(await page.locator("#detailsTab").getAttribute("hidden"), null);

    await page.addScriptTag({path: require.resolve("axe-core/axe.min.js")});
    const violations = await page.evaluate(async () => {
        const result = await axe.run(document, {
            runOnly: {type: "tag", values: ["wcag2a", "wcag2aa"]}
        });
        return result.violations.filter((item) =>
            item.impact === "serious" || item.impact === "critical"
        ).map((item) => ({
            id: item.id,
            impact: item.impact,
            nodes: item.nodes.map((node) => ({
                target: node.target,
                summary: node.failureSummary
            }))
        }));
    });
    assert.deepStrictEqual(violations, []);
    await page.close();
});

test("亮色、暗色和窄屏布局生成非空视觉证据", async () => {
    const {page} = await openExample("alt_roundtrip.json");
    const outputDir = path.join(repoRoot, "output", "playwright");
    fs.mkdirSync(outputDir, {recursive: true});
    const light = path.join(outputDir, "stack-visualizer-light.png");
    const dark = path.join(outputDir, "stack-visualizer-dark.png");
    const narrow = path.join(outputDir, "stack-visualizer-narrow.png");
    await page.screenshot({path: light, fullPage: false});
    await page.locator("#themeMode").selectOption("dark");
    assert.strictEqual(await page.locator("body").getAttribute("data-theme"), "dark");
    await page.screenshot({path: dark, fullPage: false});
    await page.setViewportSize({width: 600, height: 1000});
    const columns = await page.locator(".app").evaluate((element) =>
        getComputedStyle(element).gridTemplateColumns
    );
    assert(!columns.includes(" "), `narrow layout should use one column: ${columns}`);
    await page.screenshot({path: narrow, fullPage: false});
    for (const file of [light, dark, narrow]) {
        assert(fs.statSync(file).size > 10000, `${path.basename(file)} is unexpectedly blank`);
    }
    await page.close();
});

test("真实 buildWebviewHtml nonce CSP 下虚拟 spacer 和事件轨仍有布局", async () => {
    const trace = largeTrace(5, 1000);
    trace.steps[4].error = "synthetic event";
    const html = await buildRealWebviewHtml(trace);
    assert.match(html, /Content-Security-Policy/);
    assert(!/style-src[^>]*unsafe-inline/.test(html));
    assert(!/script-src[^>]*unsafe-inline/.test(html));

    const page = await context.newPage();
    const consoleErrors = [];
    const pageErrors = [];
    page.on("console", (message) => {
        if (message.type() === "error") consoleErrors.push(message.text());
    });
    page.on("pageerror", (error) => pageErrors.push(error.message));
    await page.setContent(html, {waitUntil: "load"});
    await page.locator("#mainStackList .stack-spacer").first().waitFor({state: "attached"});
    assert.strictEqual(await page.locator("[style]").count(), 0);
    const styleNonces = await page.locator("style").evaluateAll((elements) =>
        elements.map((element) => element.nonce)
    );
    assert(styleNonces.length >= 4);
    assert.strictEqual(new Set(styleNonces).size, 1);
    assert(styleNonces[0]);

    const spacerHeights = await page.locator("#mainStackList .stack-spacer")
        .evaluateAll((elements) => elements.map((element) =>
            Number.parseFloat(getComputedStyle(element).height)
        ));
    assert(spacerHeights.some((height) => height > 1000), String(spacerHeights));
    const eventPositions = await page.locator("#eventRail .event-dot")
        .evaluateAll((elements) => elements.map((element) =>
            element.getBoundingClientRect().left
        ));
    assert(eventPositions.length >= 2);
    assert(new Set(eventPositions.map((value) => Math.round(value))).size >= 2);
    await page.locator("#mainStackList").evaluate((element) => {
        element.scrollTop = element.scrollHeight;
        element.dispatchEvent(new Event("scroll"));
    });
    await page.waitForFunction(() =>
        document.querySelector("#mainStackList").textContent.includes("large-0")
    );
    assert.deepStrictEqual(pageErrors, []);
    assert.deepStrictEqual(
        consoleErrors.filter((message) => /content security policy|inline style/i.test(message)),
        []
    );
    await page.close();
});

test("Webview 源文件优先 step.sourceFile、再 step.source.file、最后顶层 fallback", async () => {
    const {page} = await openExample("alt_roundtrip.json");
    await page.evaluate(() => {
        window.__apcMessages.length = 0;
        loadTraceJson({
            format: "apc-stack-trace",
            source: {file: "main.ct", lines: ["one", "two", "three"]},
            steps: [
                {
                    step: 0,
                    pc: 0,
                    opcode: "OP_0",
                    sourceFile: "imported.ct",
                    source: {file: "nested-ignored.ct", line: 1}
                },
                {
                    step: 1,
                    pc: 1,
                    opcode: "OP_1",
                    source: {file: "nested.ct", line: 2}
                },
                {step: 2, pc: 2, opcode: "OP_2", sourceLine: 3}
            ]
        });
    });
    for (const [index, expected] of [
        [0, "imported.ct"],
        [1, "nested.ct"],
        [2, "main.ct"]
    ]) {
        await page.locator("#timeline").evaluate((element, value) => {
            element.value = String(value);
            element.dispatchEvent(new Event("input", {bubbles: true}));
        }, index);
        assert.strictEqual(await page.locator("#sourceTitle").textContent(), expected);
        assert((await page.locator("#metricSource").textContent()).includes(expected));
        await page.locator("#openSourceBtn").click();
    }
    const files = await page.evaluate(() => window.__apcMessages.map((item) => item.file));
    assert.deepStrictEqual(files, ["imported.ct", "nested.ct", "main.ct"]);
    await page.close();
});
