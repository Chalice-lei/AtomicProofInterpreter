#!/usr/bin/env node

const fs = require("fs");
const http = require("http");
const path = require("path");

const extensionRoot = path.resolve(__dirname, "..");
const repoRoot = path.resolve(extensionRoot, "..", "..");
const outputDir = path.join(repoRoot, "output", "playwright");
const { chromium } = requirePlaywright();

function requirePlaywright() {
  const candidates = [
    "playwright-core",
    path.join(extensionRoot, "node_modules", "playwright-core")
  ];

  for (const candidate of candidates) {
    try {
      return require(candidate);
    } catch (error) {
      if (error.code !== "MODULE_NOT_FOUND") {
        throw error;
      }
    }
  }

  throw new Error("Missing playwright-core. Run `npm install` in tools/vscode_stack_visualizer first.");
}

function assert(condition, message) {
  if (!condition) {
    throw new Error(message);
  }
}

function contentType(file) {
  if (file.endsWith(".html")) return "text/html; charset=utf-8";
  if (file.endsWith(".json")) return "application/json; charset=utf-8";
  if (file.endsWith(".js")) return "text/javascript; charset=utf-8";
  if (file.endsWith(".css")) return "text/css; charset=utf-8";
  return "text/plain; charset=utf-8";
}

function startServer() {
  const server = http.createServer((req, res) => {
    const url = new URL(req.url, "http://127.0.0.1");
    const requested = path.normalize(decodeURIComponent(url.pathname)).replace(/^(\.\.[/\\])+/, "");
    const file = path.join(repoRoot, requested);
    if (!file.startsWith(repoRoot) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
      res.writeHead(404);
      res.end("Not found");
      return;
    }
    res.writeHead(200, { "Content-Type": contentType(file) });
    fs.createReadStream(file).pipe(res);
  });

  return new Promise((resolve) => {
    server.listen(0, "127.0.0.1", () => resolve(server));
  });
}

async function launchBrowser() {
  const executableCandidates = [
    process.env.GOOGLE_CHROME_BIN,
    process.env.CHROME_BIN,
    "/usr/bin/google-chrome",
    "/usr/bin/google-chrome-stable",
    "/usr/bin/chromium-browser",
    "/usr/bin/chromium",
    "/opt/google/chrome/chrome"
  ].filter(Boolean);

  for (const executablePath of executableCandidates) {
    if (fs.existsSync(executablePath)) {
      return chromium.launch({ headless: true, executablePath });
    }
  }

  try {
    return await chromium.launch({ headless: true, channel: "chrome" });
  } catch (error) {
    return chromium.launch({ headless: true });
  }
}

(async () => {
  fs.mkdirSync(outputDir, { recursive: true });
  const server = await startServer();
  const port = server.address().port;
  let browser;
  try {
    browser = await launchBrowser();
    const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
    await page.goto(
      `http://127.0.0.1:${port}/tools/vscode_stack_visualizer/stack_visualizer/index.html?trace=/examples/stack_traces/alt_roundtrip.json`,
      { waitUntil: "networkidle" }
    );

    await page.getByRole("tab", { name: "Details" }).click();
    await page.getByText("Trace Diagnostics").waitFor();
    assert((await page.textContent("body")).includes("Trace metadata looks complete."), "diagnostics not rendered");

    await page.getByRole("tab", { name: "Explanation" }).click();
    assert((await page.textContent("body")).includes("Step Explanation"), "step explanation not rendered");

    await page.getByRole("button", { name: "Next Event" }).click();
    assert((await page.textContent("body")).includes("Moves 0x0a int=10 id=e2 from main to alt."), "move explanation missing");

    await page.getByRole("tab", { name: "JSON" }).click();
    await page.getByRole("button", { name: "Show JSON" }).click();
    assert((await page.textContent("body")).includes("Step 2 JSON"), "JSON did not expand");
    assert((await page.textContent("body")).includes("OP_TOALTSTACK"), "expanded JSON missing opcode");
    await page.getByRole("button", { name: "Hide JSON" }).click();
    assert((await page.textContent("body")).includes("Collapsed to keep long traces responsive"), "JSON did not collapse");

    await page.getByRole("tab", { name: "Explanation" }).click();
    await page.getByRole("button", { name: "Copy MD" }).click();
    assert((await page.textContent("body")).includes("Copied"), "Copy MD status missing");

    await page.getByRole("button", { name: "Copy Trace MD" }).click();
    assert((await page.textContent("body")).includes("Copied"), "Copy Trace MD status missing");

    await page.getByRole("button", { name: "Present" }).click();
    assert(await page.locator("body.presentation-mode").count() === 1, "presentation mode did not enable");
    await page.getByRole("button", { name: "Exit Present" }).click();
    assert(await page.locator("body.presentation-mode").count() === 0, "presentation mode did not disable");

    await page.evaluate(() => {
      const values = Array.from({ length: 220 }, (_item, index) => ({
        byteLength: 1,
        depth: index,
        elementId: "v" + index,
        hex: "0x" + index.toString(16).padStart(2, "0"),
        index,
        int: index,
        intString: String(index),
        originStack: "main",
        originStep: 0
      }));
      loadTraceJson({
        format: "apc-stack-trace",
        source: {
          file: "synthetic_large_stack.ct",
          lines: ["return synthetic;"]
        },
        steps: [{
          step: 0,
          pc: 0,
          opcode: "OP_SYNTHETIC_LARGE_STACK",
          functionName: "synthetic_large_stack",
          sourceLine: 1,
          mainStackBefore: [],
          mainStackAfter: values,
          altStackBefore: [],
          altStackAfter: [],
          effects: {
            mainStack: {
              pushed: values,
              popped: [],
              reordered: false,
              sizeBefore: 0,
              sizeAfter: values.length
            },
            altStack: {
              pushed: [],
              popped: [],
              reordered: false,
              sizeBefore: 0,
              sizeAfter: 0
            },
            moves: []
          }
        }]
      });
    });
    await page.getByText("Virtualized: showing rows").waitFor();
    assert((await page.textContent("#mainStackList")).includes("of 220"), "virtualized stack summary missing");

    await page.screenshot({
      fullPage: false,
      path: path.join(outputDir, "atomicproof-stack-visualizer-browser-smoke.png")
    });
    console.log("browser smoke ok");
  } finally {
    if (browser) {
      await browser.close();
    }
    server.close();
  }
})().catch((error) => {
  console.error(error.stack || error.message);
  process.exit(1);
});
