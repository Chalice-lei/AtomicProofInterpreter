#!/usr/bin/env node

const path = require("path");
const { runTests } = require("@vscode/test-electron");

async function main()
{
    const extensionDevelopmentPath = path.resolve(__dirname, "..");
    const extensionTestsPath = path.resolve(__dirname, "extension_smoke_runner.js");
    const launchArgs = ["--disable-extensions"];

    await runTests({
        extensionDevelopmentPath,
        extensionTestsPath,
        launchArgs
    });
}

main().catch((error) => {
    console.error(error.stack || error.message);
    process.exit(1);
});
