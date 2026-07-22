"use strict";

const fs = require("fs");
const http = require("http");
const path = require("path");

function contentType(file)
{
    if (file.endsWith(".html")) return "text/html; charset=utf-8";
    if (file.endsWith(".json")) return "application/json; charset=utf-8";
    if (file.endsWith(".js")) return "text/javascript; charset=utf-8";
    if (file.endsWith(".css")) return "text/css; charset=utf-8";
    return "text/plain; charset=utf-8";
}

function startServer(root)
{
    const resolvedRoot = path.resolve(root);
    const server = http.createServer((request, response) => {
        try {
            const url = new URL(request.url, "http://127.0.0.1");
            const pathname = decodeURIComponent(url.pathname);
            const file = path.resolve(resolvedRoot, "." + pathname);
            if (file !== resolvedRoot && !file.startsWith(resolvedRoot + path.sep)) {
                response.writeHead(403);
                response.end("Forbidden");
                return;
            }
            if (!fs.existsSync(file) || !fs.statSync(file).isFile()) {
                response.writeHead(404);
                response.end("Not found");
                return;
            }
            response.writeHead(200, {
                "Content-Type": contentType(file),
                "X-Content-Type-Options": "nosniff"
            });
            fs.createReadStream(file).pipe(response);
        } catch (error) {
            response.writeHead(400);
            response.end(error.message);
        }
    });
    return new Promise((resolve, reject) => {
        server.once("error", reject);
        server.listen(0, "127.0.0.1", () => resolve(server));
    });
}

async function launchChromium()
{
    let chromium;
    try {
        ({chromium} = require("playwright-core"));
    } catch (error) {
        throw new Error(
            `playwright-core is required for core Webview tests: ${error.message}`
        );
    }
    const candidates = [
        process.env.GOOGLE_CHROME_BIN,
        process.env.CHROME_BIN,
        "/usr/bin/google-chrome",
        "/usr/bin/google-chrome-stable",
        "/usr/bin/chromium-browser",
        "/usr/bin/chromium",
        "/opt/google/chrome/chrome"
    ].filter(Boolean);
    for (const executablePath of candidates) {
        if (fs.existsSync(executablePath)) {
            return chromium.launch({headless: true, executablePath});
        }
    }
    try {
        return await chromium.launch({headless: true});
    } catch (error) {
        throw new Error(
            "Chromium is required for core Webview tests. " +
            "Run `npx playwright install chromium`.\n" + error.message
        );
    }
}

async function closeServer(server)
{
    if (!server) return;
    await new Promise((resolve) => server.close(resolve));
}

module.exports = {closeServer, launchChromium, startServer};
