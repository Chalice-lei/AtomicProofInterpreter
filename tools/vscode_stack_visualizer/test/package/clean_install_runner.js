"use strict";

const assert = require("assert");
const fs = require("fs");
const path = require("path");
const vscode = require("vscode");

async function run()
{
    const extension = vscode.extensions.getExtension("atomicproof.atomicproof-stack-visualizer");
    assert(extension, "packaged extension is not installed in clean Extension Host");
    const sourceRoot = path.resolve(process.env.APC_PACKAGED_EXTENSION_ROOT);
    assert.notStrictEqual(path.resolve(extension.extensionPath), sourceRoot);
    assert(
        path.resolve(extension.extensionPath).startsWith(
            path.resolve(process.env.APC_PACKAGED_EXTENSIONS_DIR)
        ),
        `extension loaded outside clean extensions dir: ${extension.extensionPath}`
    );
    await extension.activate();
    const commands = await vscode.commands.getCommands(true);
    for (const name of extension.packageJSON.contributes.commands.map((item) => item.command)) {
        assert(commands.includes(name), `${name} missing after VSIX install`);
    }
    assert(fs.existsSync(path.join(
        extension.extensionPath,
        "schemas",
        "apc-stack-trace.schema.json"
    )));
    assert(fs.existsSync(path.join(
        extension.extensionPath,
        "stack_visualizer",
        "index.html"
    )));
}

module.exports = {run};
