#!/usr/bin/env node
"use strict";

const assert = require("assert");
const fs = require("fs");
const os = require("os");
const path = require("path");
const JSZip = require("jszip");
const {runTests, runVSCodeCommand} = require("@vscode/test-electron");
const {runProcess} = require("../helpers/process");

const extensionRoot = path.resolve(__dirname, "..", "..");

async function writeHarness(directory)
{
    await fs.promises.mkdir(directory, {recursive: true});
    await fs.promises.writeFile(path.join(directory, "package.json"), JSON.stringify({
        name: "apc-vsix-install-harness",
        displayName: "APC VSIX Install Harness",
        version: "0.0.1",
        publisher: "atomicproof-tests",
        engines: {vscode: "^1.86.0"},
        main: "./extension.js",
        activationEvents: ["*"]
    }), "utf8");
    await fs.promises.writeFile(
        path.join(directory, "extension.js"),
        "exports.activate = function () {}; exports.deactivate = function () {};\n",
        "utf8"
    );
}

async function main()
{
    const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "apc-vsix-"));
    const vsix = path.join(tempDir, "atomicproof-stack-visualizer.vsix");
    const extensionsDir = path.join(tempDir, "extensions");
    const userDataDir = path.join(tempDir, "user-data");
    const harnessDir = path.join(tempDir, "harness");
    try {
        const packed = await runProcess(process.execPath, [
            path.join(extensionRoot, "scripts", "package_vsix.js"),
            "--out",
            vsix
        ], {cwd: extensionRoot, timeoutMs: 15000});
        assert.strictEqual(packed.code, 0, packed.stderr || packed.stdout);
        console.log("package stage: VSIX created");
        assert(fs.statSync(vsix).size > 1000, "VSIX is unexpectedly small");

        const zip = await JSZip.loadAsync(fs.readFileSync(vsix));
        const files = Object.keys(zip.files).filter((name) => !zip.files[name].dir).sort();
        assert.deepStrictEqual(files, [
            "[Content_Types].xml",
            "extension.vsixmanifest",
            "extension/README.md",
            "extension/TESTING.md",
            "extension/extension.js",
            "extension/language-configuration.json",
            "extension/package.json",
            "extension/schemas/apc-stack-trace.schema.json",
            "extension/stack_visualizer/index.html"
        ]);
        assert(!files.some((name) =>
            name.includes("node_modules") ||
            name.includes("test/") ||
            name.endsWith(".log") ||
            name.includes(".git")
        ));

        const manifest = await zip.file("extension.vsixmanifest").async("string");
        assert(manifest.includes('Id="atomicproof-stack-visualizer"'));
        assert(manifest.includes('Publisher="atomicproof"'));
        assert(manifest.includes('Version="0.1.0"'));
        const packaged = JSON.parse(await zip.file("extension/package.json").async("string"));
        assert.strictEqual(packaged.main, "./extension.js");
        assert.strictEqual(packaged.contributes.commands.length, 8);
        assert.deepStrictEqual(
            packaged.contributes.debuggers.map((item) => item.type),
            ["atomicproof-trace", "atomicproof-live"]
        );
        console.log("package stage: VSIX content verified");

        await fs.promises.mkdir(extensionsDir, {recursive: true});
        await fs.promises.mkdir(userDataDir, {recursive: true});
        const cliEnv = {...process.env};
        delete cliEnv.VSCODE_IPC_HOOK_CLI;
        delete cliEnv.VSCODE_CLI_REQUIRE_TOKEN;
        const cliOptions = {
            version: process.env.VSCODE_TEST_VERSION || "stable",
            spawn: {cwd: extensionRoot, env: cliEnv}
        };
        console.log("package stage: installing VSIX");
        await runVSCodeCommand([
            "--install-extension",
            vsix,
            "--force",
            "--extensions-dir",
            extensionsDir,
            "--user-data-dir",
            userDataDir
        ], cliOptions);
        console.log("package stage: VSIX installed");
        const listed = await runVSCodeCommand([
            "--list-extensions",
            "--show-versions",
            "--extensions-dir",
            extensionsDir,
            "--user-data-dir",
            userDataDir
        ], cliOptions);
        console.log("package stage: extensions listed");
        assert.match(listed.stdout, /atomicproof\.atomicproof-stack-visualizer@0\.1\.0/i);

        await writeHarness(harnessDir);
        process.env.APC_PACKAGED_EXTENSION_ROOT = extensionRoot;
        process.env.APC_PACKAGED_EXTENSIONS_DIR = extensionsDir;
        delete process.env.ELECTRON_RUN_AS_NODE;
        delete process.env.VSCODE_IPC_HOOK_CLI;
        delete process.env.VSCODE_CLI_REQUIRE_TOKEN;
        console.log("package stage: starting clean Extension Host");
        await runTests({
            version: process.env.VSCODE_TEST_VERSION || "stable",
            extensionDevelopmentPath: harnessDir,
            extensionTestsPath: path.join(__dirname, "clean_install_runner.js"),
            launchArgs: [
                "--disable-gpu",
                "--extensions-dir",
                extensionsDir,
                "--user-data-dir",
                userDataDir
            ]
        });
        console.log("VSIX content and clean Extension Host install ok");
    } finally {
        fs.rmSync(tempDir, {recursive: true, force: true});
    }
}

main().catch((error) => {
    console.error(error.stack || error.message);
    process.exit(1);
});
