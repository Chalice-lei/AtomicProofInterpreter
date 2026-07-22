const vscode = require("vscode");
const fs = require("fs");
const path = require("path");
const cp = require("child_process");
const os = require("os");

const output = vscode.window.createOutputChannel(
    "AtomicProof Stack Visualizer"
);
const CONFIG_SECTION = "atomicProofStackVisualizer";
const TRACE_DEBUG_TYPE = "atomicproof-trace";
const LIVE_DEBUG_TYPE = "atomicproof-live";
const LAST_TRACE_KEY = "lastTracePath";
const LAST_FUNCTION_KEY = "lastFunctionName";
const LAST_ARGS_KEY = "lastArguments";
const AUTO_SAVE_STATUS_DELAY_MS = 2500;

const autoRunSaveTimers = new Map();
const autoRunLocks = new Set();
const tracePanels = new Map();
const liveSessionsByContract = new Map();
const liveSessionContracts = new Map();
const liveDebugConfigsByContract = new Map();

let statusBarItem;
let statusResetTimer;

function activate(context)
{
    context.subscriptions.push(output);
    context.subscriptions.push(createStatusBarItem());
    context.subscriptions.push(
        vscode.commands.registerCommand(
            "atomicProofStackVisualizer.openTrace",
            () => openTracePicker(context)
        )
    );
    context.subscriptions.push(
        vscode.commands.registerCommand(
            "atomicProofStackVisualizer.openActiveTrace",
            (uri) => openActiveTrace(context, uri)
        )
    );
    context.subscriptions.push(
        vscode.commands.registerCommand(
            "atomicProofStackVisualizer.generateTrace",
            (uri) => generateTrace(context, uri)
        )
    );
    context.subscriptions.push(
        vscode.commands.registerCommand(
            "atomicProofStackVisualizer.openLastTrace",
            () => openLastTrace(context)
        )
    );
    context.subscriptions.push(
        vscode.commands.registerCommand(
            "atomicProofStackVisualizer.debugActiveTrace",
            (uri) => debugActiveTrace(context, uri)
        )
    );
    context.subscriptions.push(
        vscode.commands.registerCommand(
            "atomicProofStackVisualizer.debugLiveContract",
            (uri) => debugLiveContract(context, uri)
        )
    );
    context.subscriptions.push(
        vscode.commands.registerCommand(
            "atomicProofStackVisualizer.toggleAutoDebugOnSave",
            () => toggleAutoDebugOnSave()
        )
    );
    context.subscriptions.push(
        vscode.commands.registerCommand(
            "atomicProofStackVisualizer.restartCurrentLiveDebug",
            () => restartCurrentLiveDebug(context)
        )
    );
    registerAutoRunOnSave(context);
    registerLiveDebugSessionTracking(context);
    context.subscriptions.push(
        vscode.workspace.onDidChangeConfiguration((event) => {
            if (event.affectsConfiguration(`${CONFIG_SECTION}.autoRunOnSave`) ||
                event.affectsConfiguration(`${CONFIG_SECTION}.openBeside`)) {
                updateStatusBarItem();
            }
        })
    );
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider(
            TRACE_DEBUG_TYPE,
            new AtomicProofTraceDebugConfigurationProvider()
        )
    );
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory(
            TRACE_DEBUG_TYPE,
            new AtomicProofTraceDebugAdapterFactory()
        )
    );
    context.subscriptions.push(
        vscode.debug.registerDebugConfigurationProvider(
            LIVE_DEBUG_TYPE,
            new AtomicProofLiveDebugConfigurationProvider()
        )
    );
    context.subscriptions.push(
        vscode.debug.registerDebugAdapterDescriptorFactory(
            LIVE_DEBUG_TYPE,
            new AtomicProofLiveDebugAdapterFactory()
        )
    );
}

function deactivate()
{}

async function openTracePicker(context)
{
    const files = await vscode.window.showOpenDialog({
        canSelectFiles: true,
        canSelectFolders: false,
        canSelectMany: false,
        filters: {
            "Stack Trace JSON": ["json"]
        },
        title: "Open AtomicProof stack_trace.json"
    });

    if (!files || files.length === 0) {
        return;
    }

    await openTrace(context, files[0]);
}

async function openActiveTrace(context, uri)
{
    const target = uri || vscode.window.activeTextEditor?.document.uri;
    if (!target || target.scheme !== "file") {
        vscode.window.showWarningMessage(
            "Open a stack_trace.json file or choose one from the file picker."
        );
        return openTracePicker(context);
    }

    await openTrace(context, target);
}

async function openTrace(context, traceUri, options = {})
{
    try {
        const traceText = await fs.promises.readFile(traceUri.fsPath, "utf8");
        const trace = parseJson(traceText, traceUri.fsPath);
        if (isCompiledArtifact(trace)) {
            await offerGenerateTraceFromArtifact(context, traceUri, trace, {
                openAfterGenerate: true
            });
            return;
        }
        validateTrace(trace);
        const templatePath = getVisualizerTemplatePath(context);
        const key = normalizeFsPath(traceUri.fsPath);
        const existingPanel = tracePanels.get(key);
        if (existingPanel) {
            existingPanel.title = `Stack Trace: ${path.basename(traceUri.fsPath)}`;
            existingPanel.webview.html = await buildWebviewHtml(
                context,
                existingPanel.webview,
                trace,
                traceUri.fsPath,
                templatePath
            );
            existingPanel.reveal(getOpenColumn(), Boolean(options.preserveFocus));
            return;
        }

        const panel = vscode.window.createWebviewPanel(
            "atomicProofStackVisualizer",
            `Stack Trace: ${path.basename(traceUri.fsPath)}`,
            getWebviewShowOptions(options),
            {
                enableScripts: true,
                retainContextWhenHidden: true,
                localResourceRoots: [
                    vscode.Uri.file(path.dirname(traceUri.fsPath)),
                    vscode.Uri.file(path.dirname(templatePath))
                ]
            }
        );

        panel.webview.html = await buildWebviewHtml(
            context,
            panel.webview,
            trace,
            traceUri.fsPath,
            templatePath
        );

        panel.webview.onDidReceiveMessage(
            (message) => handleWebviewMessage(context, traceUri.fsPath, message),
            undefined,
            context.subscriptions
        );
        tracePanels.set(key, panel);
        panel.onDidDispose(
            () => {
                if (tracePanels.get(key) === panel) {
                    tracePanels.delete(key);
                }
            },
            undefined,
            context.subscriptions
        );
    } catch (error) {
        vscode.window.showErrorMessage(
            `Failed to open stack trace: ${error.message}`
        );
    }
}

function validateTrace(trace)
{
    if (!isStackTrace(trace)) {
        throw new Error("selected JSON is not an apc-stack-trace file");
    }
}

function isStackTrace(value)
{
    return Boolean(value && value.format === "apc-stack-trace" &&
        Array.isArray(value.steps));
}

function isCompiledArtifact(value)
{
    return Boolean(value && value.lock &&
        (typeof value.lock.asm === "string" ||
            typeof value.lock.hex === "string") &&
        (Array.isArray(value.functions) || Array.isArray(value.abi)));
}

function parseJson(text, filePath)
{
    try {
        return JSON.parse(text);
    } catch (error) {
        throw new Error(`${path.basename(filePath)} is not valid JSON: ${error.message}`);
    }
}

async function offerGenerateTraceFromArtifact(context, artifactUri, artifact, options = {})
{
    const contractUri = await findArtifactContractUri(artifactUri, artifact);
    const basename = path.basename(artifactUri.fsPath);
    if (!contractUri) {
        vscode.window.showErrorMessage(
            `${basename} is a compiled contract JSON, not a stack trace. ` +
            "Open the matching .ct file and run AtomicProof: Generate Stack Trace and Visualize."
        );
        return undefined;
    }

    const actionLabel = options.actionLabel || "Generate Trace";
    const choice = await vscode.window.showInformationMessage(
        `${basename} is a compiled contract JSON. Generate a stack trace from ` +
            `${path.basename(contractUri.fsPath)} first?`,
        actionLabel,
        "Open Source"
    );

    if (choice === "Open Source") {
        const document = await vscode.workspace.openTextDocument(contractUri);
        await vscode.window.showTextDocument(document);
        return undefined;
    }
    if (choice !== actionLabel) {
        return undefined;
    }

    const artifactFunction = getArtifactFunctions(artifact)[0];
    return generateTraceForContract(context, contractUri, {
        functionName: artifactFunction?.name,
        params: artifactFunction?.params,
        openAfterGenerate: options.openAfterGenerate
    });
}

async function findArtifactContractUri(artifactUri, artifact)
{
    const references = getArtifactSourceReferences(artifactUri, artifact);
    const direct = getArtifactContractCandidates(artifactUri.fsPath, references)
        .find((candidate) => fs.existsSync(candidate));
    if (direct) {
        return vscode.Uri.file(direct);
    }

    const workspaceMatches = await findWorkspaceContractMatches(references);
    if (workspaceMatches.length === 1) {
        return workspaceMatches[0];
    }
    if (workspaceMatches.length > 1) {
        const picked = await vscode.window.showQuickPick(
            workspaceMatches.map((uri) => ({
                label: path.basename(uri.fsPath),
                description: vscode.workspace.asRelativePath(uri),
                uri
            })),
            {
                title: "Choose contract source",
                placeHolder: "Multiple matching .ct files were found"
            }
        );
        return picked?.uri;
    }
    return undefined;
}

function getArtifactSourceReferences(artifactUri, artifact)
{
    const references = [];
    const add = (value) => {
        const text = String(value || "").trim();
        if (text && !references.includes(text)) {
            references.push(text);
        }
    };

    add(artifact?.metadata?.source_file);
    add(artifact?.metadata?.sourceFile);
    add(artifact?.source_file);
    add(artifact?.sourceFile);
    add(path.basename(artifactUri.fsPath, path.extname(artifactUri.fsPath)));
    return references;
}

function getArtifactContractCandidates(artifactPath, references)
{
    const workspaceRoot = getWorkspaceRoot();
    const candidates = [];
    const seen = new Set();
    const push = (candidate) => {
        if (!candidate) {
            return;
        }
        const normalized = path.normalize(candidate);
        if (!seen.has(normalized)) {
            seen.add(normalized);
            candidates.push(normalized);
        }
    };

    for (const reference of references) {
        for (const candidate of referenceContractNames(reference)) {
            if (path.isAbsolute(candidate)) {
                push(candidate);
            } else {
                push(path.join(path.dirname(artifactPath), candidate));
                if (workspaceRoot) {
                    push(path.join(workspaceRoot, candidate));
                }
            }
        }
    }
    return candidates;
}

function referenceContractNames(reference)
{
    if (!reference) {
        return [];
    }
    const ext = path.extname(reference);
    return ext === ".ct" ? [reference] : [reference + ".ct"];
}

async function findWorkspaceContractMatches(references)
{
    if (!vscode.workspace.workspaceFolders?.length) {
        return [];
    }

    const basenames = [...new Set(references
        .map((reference) => path.basename(reference, path.extname(reference)))
        .filter(Boolean))];
    const matches = [];
    const seen = new Set();
    for (const basename of basenames) {
        const found = await vscode.workspace.findFiles(
            `**/${basename}.ct`,
            "**/{node_modules,build}/**",
            20
        );
        for (const uri of found) {
            if (!seen.has(uri.fsPath)) {
                seen.add(uri.fsPath);
                matches.push(uri);
            }
        }
    }
    return matches;
}

function getArtifactFunctions(artifact)
{
    const functions = [];
    const seen = new Set();
    const add = (item) => {
        const name = String(item?.name || "").trim();
        if (!name || seen.has(name)) {
            return;
        }
        seen.add(name);
        functions.push({
            name,
            params: Array.isArray(item.params) ? item.params : []
        });
    };

    for (const item of Array.isArray(artifact?.functions) ? artifact.functions : []) {
        add(item);
    }
    for (const item of Array.isArray(artifact?.abi) ? artifact.abi : []) {
        if (!item.type || item.type === "function") {
            add(item);
        }
    }
    return functions;
}

async function buildWebviewHtml(context, webview, trace, tracePath, templatePath)
{
    const nonce = getNonce();
    let html = await fs.promises.readFile(templatePath, "utf8");

    const injectedTrace = JSON.stringify(trace)
        .replace(/</g, "\\u003c")
        .replace(/\u2028/g, "\\u2028")
        .replace(/\u2029/g, "\\u2029");
    const startupOptions = JSON.stringify(getWebviewStartupOptions())
        .replace(/</g, "\\u003c")
        .replace(/\u2028/g, "\\u2028")
        .replace(/\u2029/g, "\\u2029");

    html = html.replace(
        "<head>",
        `<head>\n  <meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src ${webview.cspSource} data:; style-src ${webview.cspSource} 'nonce-${nonce}'; script-src 'nonce-${nonce}'; font-src ${webview.cspSource};">`
    );
    html = html.replace(/<style>/g, `<style nonce="${nonce}">`);
    html = html.replace(/<script>/g, `<script nonce="${nonce}">`);

    html = html.replace(
        "<title>APC Stack Trace Visualizer</title>",
        "<title>AtomicProof Stack Trace</title>"
    );

    html = html.replace(
        '<input class="file-input" id="traceFile" type="file" accept="application/json,.json" aria-label="Choose stack trace JSON">',
        '<button class="button" id="traceFile" title="Loaded from VS Code" type="button" disabled>VS Code Trace</button>'
    );

    html = html.replace(
        "loadTraceFromQuery();",
        `loadTraceJson(${injectedTrace});\n` +
            `applyStartupOptions(${startupOptions});\n` +
            `document.title = "AtomicProof Stack Trace - ${escapeForJsString(path.basename(tracePath))}";`
    );

    html = html.replace(
        "</body>",
        `<script nonce="${nonce}">console.info("AtomicProof VS Code trace loaded: ${escapeForJsString(tracePath)}");</script>\n</body>`
    );

    // The reused visualizer is self-contained; keep URIs untouched except for
    // any future relative media references that may be added to the template.
    return html.replace(
        /(href|src)="(?!data:|https?:|#)([^"]+)"/g,
        (_match, attr, rawUrl) => {
            const localPath = path.join(path.dirname(templatePath), rawUrl);
            const uri = webview.asWebviewUri(vscode.Uri.file(localPath));
            return `${attr}="${uri}"`;
        }
    );
}

function escapeForJsString(value)
{
    return String(value)
        .replace(/\\/g, "\\\\")
        .replace(/"/g, "\\\"")
        .replace(/`/g, "\\`")
        .replace(/\$/g, "\\$")
        .replace(/\r/g, "\\r")
        .replace(/\n/g, "\\n");
}

async function openLastTrace(context)
{
    const lastTracePath = context.workspaceState.get(LAST_TRACE_KEY);
    if (!lastTracePath) {
        vscode.window.showWarningMessage("No generated stack trace has been recorded yet.");
        return;
    }
    await openTrace(context, vscode.Uri.file(lastTracePath));
}

async function generateTrace(context, uri)
{
    const workspaceRoot = getWorkspaceRoot();
    if (!workspaceRoot) {
        vscode.window.showWarningMessage(
            "Open the AtomicProofInterpreter folder before generating a trace."
        );
        return;
    }

    const contractUri = await chooseContractUri(workspaceRoot, uri);
    if (!contractUri) {
        return;
    }

    return generateTraceForContract(context, contractUri, {
        openAfterGenerate: getConfig().get("autoOpenGeneratedTrace") !== false
    });
}

async function generateTraceForContract(context, contractUri, options = {})
{
    const workspaceRoot = getWorkspaceRoot();
    if (!workspaceRoot) {
        const error = new Error(
            "Open the AtomicProofInterpreter folder before generating a trace."
        );
        if (options.throwOnError) {
            throw error;
        }
        vscode.window.showWarningMessage(error.message);
        return undefined;
    }

    const functionName = await resolveTraceFunctionName(
        context,
        contractUri.fsPath,
        options
    );
    if (!functionName) {
        return undefined;
    }
    await context.workspaceState.update(LAST_FUNCTION_KEY, functionName);

    const argsText = await resolveTraceArgumentsText(context, options);
    if (argsText === undefined) {
        return undefined;
    }
    await context.workspaceState.update(LAST_ARGS_KEY, argsText);

    const selectedTrace = await resolveTraceOutputUri(
        workspaceRoot,
        contractUri.fsPath,
        options
    );

    if (!selectedTrace) {
        return undefined;
    }

    const executable = getInterpreterPath(workspaceRoot, contractUri.fsPath);

    if (!fs.existsSync(executable)) {
        const error = new Error(
            `Executable not found: ${executable}. Build the project first or set ${CONFIG_SECTION}.interpreterPath.`
        );
        output.appendLine(error.message);
        if (options.throwOnError) {
            throw error;
        }
        if (options.showErrorMessage !== false) {
            const choice = await vscode.window.showErrorMessage(
                error.message,
                "Open Settings"
            );
            if (choice === "Open Settings") {
                vscode.commands.executeCommand(
                    "workbench.action.openSettings",
                    `${CONFIG_SECTION}.interpreterPath`
                );
            }
        }
        return undefined;
    }

    const args = [
        "run",
        path.relative(workspaceRoot, contractUri.fsPath),
        functionName,
        ...splitArgs(argsText || ""),
        "--stack-trace-output",
        selectedTrace.fsPath
    ];

    if (options.clearOutput !== false) {
        output.clear();
    }
    if (options.showOutput !== false) {
        output.show(true);
    }
    output.appendLine(formatCommand(executable, args));

    try {
        await fs.promises.mkdir(path.dirname(selectedTrace.fsPath), {
            recursive: true
        });
        await vscode.window.withProgress(
            {
                location: options.progressLocation ||
                    vscode.ProgressLocation.Notification,
                title: options.progressTitle ||
                    "Generating AtomicProof stack trace",
                cancellable: options.cancellable !== false
            },
            (_progress, token) => execFile(executable, args, workspaceRoot, token)
        );
        output.appendLine("Stack trace generated.");
        await context.workspaceState.update(LAST_TRACE_KEY, selectedTrace.fsPath);
        vscode.window.setStatusBarMessage(
            `AtomicProof trace generated: ${path.basename(selectedTrace.fsPath)}`,
            5000
        );
        if (options.openAfterGenerate !== false) {
            await openTrace(context, selectedTrace, {
                preserveFocus: Boolean(options.preserveFocus)
            });
        }
        return selectedTrace;
    } catch (error) {
        output.appendLine(error.stack || error.message);
        if (options.throwOnError) {
            throw error;
        }
        if (options.showErrorMessage !== false) {
            const choice = await vscode.window.showErrorMessage(
                `Stack trace generation failed: ${error.message}`,
                "Show Output"
            );
            if (choice === "Show Output") {
                output.show(true);
            }
        }
        return undefined;
    }
}

async function resolveTraceFunctionName(context, contractPath, options)
{
    const provided = String(options.functionName || "").trim();
    if (provided) {
        return provided;
    }

    if (options.useRecentInputs) {
        const lastFunction = String(
            context.workspaceState.get(LAST_FUNCTION_KEY) || ""
        ).trim();
        const configured = String(getConfig().get("defaultFunction") || "").trim();
        if (lastFunction || configured) {
            return lastFunction || configured;
        }
    }

    return chooseFunctionName(context, contractPath, options.functionName);
}

async function resolveTraceArgumentsText(context, options)
{
    if (Object.prototype.hasOwnProperty.call(options, "argumentsText")) {
        return String(options.argumentsText ?? "");
    }

    if (options.useRecentInputs) {
        const lastArgs = context.workspaceState.get(LAST_ARGS_KEY);
        if (typeof lastArgs === "string") {
            return lastArgs;
        }
        const configured = String(getConfig().get("defaultArguments") || "");
        if (configured) {
            return configured;
        }
    }

    return vscode.window.showInputBox({
        prompt: "Function arguments, separated by spaces",
        value: getDefaultArguments(context),
        placeHolder: placeholderArguments(options.params)
    });
}

async function resolveTraceOutputUri(workspaceRoot, contractPath, options)
{
    if (options.traceUri) {
        return options.traceUri instanceof vscode.Uri
            ? options.traceUri
            : vscode.Uri.file(String(options.traceUri));
    }

    const tracePath = resolveConfiguredPath(
        getConfig().get("traceOutputPath") || "${workspaceFolder}/stack_trace.json",
        workspaceRoot,
        contractPath
    );
    if (options.promptForTracePath === false) {
        return vscode.Uri.file(tracePath);
    }

    return vscode.window.showSaveDialog({
        defaultUri: vscode.Uri.file(tracePath),
        filters: {
            "Stack Trace JSON": ["json"]
        },
        title: "Save stack trace JSON"
    });
}

function splitArgs(text)
{
    const re = /"([^"]*)"|'([^']*)'|(\S+)/g;
    const args = [];
    let match;
    while ((match = re.exec(text)) !== null) {
        args.push(match[1] ?? match[2] ?? match[3]);
    }
    return args;
}

function execFile(file, args, cwd, token)
{
    return new Promise((resolve, reject) => {
        const child = cp.spawn(file, args, {cwd});
        let settled = false;

        const finish = (callback, value) => {
            if (settled) {
                return;
            }
            settled = true;
            callback(value);
        };

        child.stdout.on("data", (chunk) => output.append(chunk.toString()));
        child.stderr.on("data", (chunk) => output.append(chunk.toString()));
        child.on("error", (error) => finish(reject, error));
        child.on("close", (code, signal) => {
            if (token?.isCancellationRequested) {
                finish(reject, new Error("Trace generation cancelled."));
            } else if (signal) {
                finish(reject, new Error(`Interpreter terminated by signal ${signal}.`));
            } else if (code === 0) {
                finish(resolve);
            } else {
                finish(reject, new Error(`Interpreter exited with code ${code}.`));
            }
        });

        token?.onCancellationRequested(() => {
            child.kill();
        });
    });
}

function registerAutoRunOnSave(context)
{
    context.subscriptions.push(
        vscode.workspace.onDidSaveTextDocument((document) => {
            if (!isContractUri(document.uri) ||
                getConfig().get("autoRunOnSave.enabled") !== true) {
                return;
            }

            const key = normalizeFsPath(document.uri.fsPath);
            const existing = autoRunSaveTimers.get(key);
            if (existing) {
                clearTimeout(existing);
            }

            const timer = setTimeout(() => {
                autoRunSaveTimers.delete(key);
                handleAutoRunOnSave(context, document.uri).catch((error) =>
                    showAutoRunError(error, getAutoRunMode())
                );
            }, getAutoRunDebounceMs());
            autoRunSaveTimers.set(key, timer);
        })
    );

    context.subscriptions.push({
        dispose() {
            for (const timer of autoRunSaveTimers.values()) {
                clearTimeout(timer);
            }
            autoRunSaveTimers.clear();
        }
    });
}

async function handleAutoRunOnSave(context, contractUri)
{
    if (!isContractUri(contractUri) ||
        getConfig().get("autoRunOnSave.enabled") !== true) {
        return;
    }

    const mode = getAutoRunMode();
    const key = `${mode}:${normalizeFsPath(contractUri.fsPath)}`;
    if (autoRunLocks.has(key)) {
        output.appendLine(
            `[Auto ${mode}] Skipped ${contractUri.fsPath}; previous run is still active.`
        );
        return;
    }

    autoRunLocks.add(key);
    showTransientStatus("$(sync~spin) AtomicProof: refreshing...");
    try {
        let didRun;
        if (mode === "live") {
            didRun = await runAutoLiveOnSave(context, contractUri);
        } else {
            didRun = await runAutoTraceOnSave(context, contractUri);
        }
        if (didRun) {
            showTransientStatus("$(check) AtomicProof: refreshed", AUTO_SAVE_STATUS_DELAY_MS);
        } else {
            updateStatusBarItem();
        }
    } catch (error) {
        showAutoRunError(error, mode);
    } finally {
        autoRunLocks.delete(key);
    }
}

async function runAutoTraceOnSave(context, contractUri)
{
    output.appendLine(`[Auto Trace] Saved ${contractUri.fsPath}`);
    const traceUri = await generateTraceForContract(context, contractUri, {
        useRecentInputs: true,
        promptForTracePath: false,
        openAfterGenerate: true,
        preserveFocus: true,
        showOutput: false,
        clearOutput: false,
        progressLocation: vscode.ProgressLocation.Window,
        progressTitle: "AtomicProof: refreshing trace",
        cancellable: false,
        showErrorMessage: false,
        throwOnError: true
    });
    if (traceUri) {
        output.appendLine(`[Auto Trace] Refreshed ${traceUri.fsPath}`);
    }
    return Boolean(traceUri);
}

async function runAutoLiveOnSave(context, contractUri)
{
    if (getConfig().get("autoRunOnSave.restartLiveDebug") === false) {
        output.appendLine("[Auto Live] Skipped because restartLiveDebug is disabled.");
        return false;
    }

    const key = normalizeFsPath(contractUri.fsPath);
    if (!liveSessionsByContract.has(key) && !liveDebugConfigsByContract.has(key)) {
        output.appendLine(
            `[Auto Live] No prior live debug session for ${contractUri.fsPath}; skipped.`
        );
        return false;
    }

    await restartLiveDebugForContract(context, contractUri.fsPath, {
        auto: true
    });
    return true;
}

function getAutoRunMode()
{
    return getConfig().get("autoRunOnSave.mode") === "live" ? "live" : "trace";
}

function getAutoRunDebounceMs()
{
    const value = Number(getConfig().get("autoRunOnSave.debounceMs"));
    return Number.isFinite(value) ? Math.max(0, value) : 600;
}

async function toggleAutoDebugOnSave()
{
    const enabled = getConfig().get("autoRunOnSave.enabled") === true;
    const target = vscode.workspace.workspaceFolders?.length
        ? vscode.ConfigurationTarget.Workspace
        : vscode.ConfigurationTarget.Global;
    await getConfig().update("autoRunOnSave.enabled", !enabled, target);
    updateStatusBarItem();
    vscode.window.setStatusBarMessage(
        `AtomicProof auto debug on save ${enabled ? "disabled" : "enabled"}.`,
        3000
    );
}

function showAutoRunError(error, mode)
{
    const message = error?.message || String(error);
    output.appendLine(`[Auto ${mode}] Failed: ${error?.stack || message}`);
    showTransientStatus("$(error) AtomicProof: error", AUTO_SAVE_STATUS_DELAY_MS);
    vscode.window.showErrorMessage(
        `AtomicProof auto ${mode} failed: ${message}`,
        "Show Output"
    ).then((choice) => {
        if (choice === "Show Output") {
            output.show(true);
        }
    });
}

function showTransientStatus(text, delayMs)
{
    if (statusResetTimer) {
        clearTimeout(statusResetTimer);
        statusResetTimer = undefined;
    }
    if (getConfig().get("autoRunOnSave.showStatus") !== false && statusBarItem) {
        statusBarItem.text = text;
        statusBarItem.tooltip = "AtomicProof auto debug on save";
        statusBarItem.command = "atomicProofStackVisualizer.toggleAutoDebugOnSave";
        statusBarItem.show();
    }
    if (delayMs !== undefined) {
        statusResetTimer = setTimeout(() => {
            statusResetTimer = undefined;
            updateStatusBarItem();
        }, delayMs);
    }
}

function registerLiveDebugSessionTracking(context)
{
    context.subscriptions.push(
        vscode.debug.onDidStartDebugSession((session) => {
            if (!isLiveDebugSession(session)) {
                return;
            }
            const record = rememberLiveDebugConfig(
                context,
                session.workspaceFolder,
                session.configuration
            );
            if (!record) {
                return;
            }
            liveSessionsByContract.set(record.key, session);
            liveSessionContracts.set(session.id, record.key);
        })
    );
    context.subscriptions.push(
        vscode.debug.onDidTerminateDebugSession((session) => {
            const key = liveSessionContracts.get(session.id);
            if (!key) {
                return;
            }
            if (liveSessionsByContract.get(key) === session) {
                liveSessionsByContract.delete(key);
            }
            liveSessionContracts.delete(session.id);
        })
    );
}

function isLiveDebugSession(session)
{
    return session?.type === LIVE_DEBUG_TYPE ||
        session?.configuration?.type === LIVE_DEBUG_TYPE;
}

function rememberLiveDebugConfig(_context, folder, config)
{
    const record = createLiveDebugRecord(folder, config);
    if (record) {
        liveDebugConfigsByContract.set(record.key, record);
    }
    return record;
}

function createLiveDebugRecord(folder, config = {})
{
    const contractPath = resolveLiveContractPath(String(config.contractPath || ""));
    if (!contractPath) {
        return undefined;
    }

    const workspaceRoot = folder?.uri.fsPath || getWorkspaceRoot() ||
        path.dirname(contractPath);
    const interpreterPath = resolveLiveInterpreterPath(
        config.interpreterPath,
        workspaceRoot,
        contractPath
    );
    const txFile = config.txFile
        ? resolveConfiguredPath(String(config.txFile), workspaceRoot, contractPath)
        : "";

    return {
        key: normalizeFsPath(contractPath),
        type: LIVE_DEBUG_TYPE,
        request: "launch",
        name: config.name || `Live Debug ${path.basename(contractPath)}`,
        contractPath,
        functionName: String(config.functionName || "").trim(),
        arguments: normalizeDebugArguments(config.arguments),
        txFile,
        interpreterPath
    };
}

async function restartCurrentLiveDebug(context)
{
    const record = getCurrentLiveDebugRecord(context);
    if (!record) {
        vscode.window.showWarningMessage(
            "No AtomicProof live VM debug session is available to restart."
        );
        return;
    }
    await restartLiveDebugForContract(context, record.contractPath, {
        auto: false
    });
}

function getCurrentLiveDebugRecord(context)
{
    const activeSession = vscode.debug.activeDebugSession;
    if (isLiveDebugSession(activeSession)) {
        const record = rememberLiveDebugConfig(
            context,
            activeSession.workspaceFolder,
            activeSession.configuration
        );
        if (record) {
            return record;
        }
    }

    const activeUri = vscode.window.activeTextEditor?.document.uri;
    if (isContractUri(activeUri)) {
        const record = liveDebugConfigsByContract.get(
            normalizeFsPath(activeUri.fsPath)
        );
        if (record) {
            return record;
        }
    }

    const runningRecords = [...liveSessionsByContract.keys()]
        .map((key) => liveDebugConfigsByContract.get(key))
        .filter(Boolean);
    if (runningRecords.length === 1) {
        return runningRecords[0];
    }

    const rememberedRecords = [...liveDebugConfigsByContract.values()];
    return rememberedRecords.length === 1 ? rememberedRecords[0] : undefined;
}

async function restartLiveDebugForContract(_context, contractPath, options = {})
{
    const key = normalizeFsPath(contractPath);
    const lockKey = `live-restart:${key}`;
    if (autoRunLocks.has(lockKey)) {
        output.appendLine(`[Auto Live] Restart already in progress for ${contractPath}.`);
        return;
    }

    const record = liveDebugConfigsByContract.get(key);
    if (!record) {
        throw new Error(`No live debug configuration has been recorded for ${contractPath}.`);
    }

    autoRunLocks.add(lockKey);
    try {
        const running = liveSessionsByContract.get(key);
        output.appendLine(
            `${options.auto ? "[Auto Live]" : "[Live Debug]"} Restarting ${record.contractPath}`
        );
        if (running) {
            const terminated = waitForDebugSessionTermination(running);
            try {
                await vscode.debug.stopDebugging(running);
            } catch (error) {
                output.appendLine(
                    `[Live Debug] Stop request failed before restart: ${error.message}`
                );
                liveSessionsByContract.delete(key);
            }
            await terminated;
        }
        await startLiveDebugRecord(record);
    } finally {
        autoRunLocks.delete(lockKey);
    }
}

async function startLiveDebugRecord(record)
{
    const folder = vscode.workspace.getWorkspaceFolder(
        vscode.Uri.file(record.contractPath)
    );
    const config = {
        type: LIVE_DEBUG_TYPE,
        request: "launch",
        name: record.name,
        contractPath: record.contractPath,
        functionName: record.functionName,
        arguments: record.arguments,
        interpreterPath: record.interpreterPath
    };
    if (record.txFile) {
        config.txFile = record.txFile;
    }
    await vscode.debug.startDebugging(folder, config);
}

function waitForDebugSessionTermination(session)
{
    return new Promise((resolve) => {
        let settled = false;
        const timeout = setTimeout(done, 3000);
        const subscription = vscode.debug.onDidTerminateDebugSession((ended) => {
            if (ended.id === session.id) {
                done();
            }
        });

        function done()
        {
            if (settled) {
                return;
            }
            settled = true;
            clearTimeout(timeout);
            subscription.dispose();
            resolve();
        }
    });
}

function getWorkspaceRoot()
{
    return vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
}

function getRepositoryRoot(context)
{
    const workspaceRoot = getWorkspaceRoot();
    if (workspaceRoot) {
        return workspaceRoot;
    }
    return path.resolve(context.extensionPath, "..", "..");
}

function getVisualizerTemplatePath(context)
{
    const candidates = [
        path.join(context.extensionPath, "stack_visualizer", "index.html")
    ];

    const templatePath = candidates.find((candidate) => fs.existsSync(candidate));
    if (!templatePath) {
        throw new Error("Bundled stack visualizer template not found in extension package.");
    }
    return templatePath;
}

async function debugActiveTrace(context, uri)
{
    const target = uri || vscode.window.activeTextEditor?.document.uri;
    if (!target || target.scheme !== "file") {
        vscode.window.showWarningMessage(
            "Open a stack_trace.json file before starting the AtomicProof trace debugger."
        );
        return;
    }

    try {
        const traceText = await fs.promises.readFile(target.fsPath, "utf8");
        const trace = parseJson(traceText, target.fsPath);
        if (isCompiledArtifact(trace)) {
            const generatedTrace = await offerGenerateTraceFromArtifact(
                context,
                target,
                trace,
                {
                    actionLabel: "Generate and Debug",
                    openAfterGenerate: false
                }
            );
            if (generatedTrace) {
                await startTraceDebug(generatedTrace);
            }
            return;
        }
        validateTrace(trace);
    } catch (error) {
        vscode.window.showErrorMessage(
            `Failed to start trace debugger: ${error.message}`
        );
        return;
    }

    await startTraceDebug(target);
}

async function startTraceDebug(traceUri)
{
    const folder = vscode.workspace.getWorkspaceFolder(traceUri);
    await vscode.debug.startDebugging(folder, {
        type: TRACE_DEBUG_TYPE,
        name: `Debug ${path.basename(traceUri.fsPath)}`,
        request: "launch",
        tracePath: traceUri.fsPath
    });
}

async function debugLiveContract(context, uri)
{
    const workspaceRoot = getWorkspaceRoot();
    if (!workspaceRoot) {
        vscode.window.showWarningMessage(
            "Open the AtomicProofInterpreter folder before starting live VM debugging."
        );
        return;
    }

    const contractUri = await chooseContractUri(workspaceRoot, uri);
    if (!contractUri) {
        return;
    }

    const functionName = await chooseFunctionName(context, contractUri.fsPath);
    if (!functionName) {
        return;
    }
    await context.workspaceState.update(LAST_FUNCTION_KEY, functionName);

    const argsText = await vscode.window.showInputBox({
        prompt: "Function arguments, separated by spaces",
        value: getDefaultArguments(context),
        placeHolder: "5"
    });
    if (argsText === undefined) {
        return;
    }
    await context.workspaceState.update(LAST_ARGS_KEY, argsText);

    const folder = vscode.workspace.getWorkspaceFolder(contractUri);
    const interpreterPath = getInterpreterPath(workspaceRoot, contractUri.fsPath);
    const config = {
        type: LIVE_DEBUG_TYPE,
        request: "launch",
        name: `Live Debug ${path.basename(contractUri.fsPath)}`,
        contractPath: contractUri.fsPath,
        functionName,
        arguments: splitArgs(argsText || ""),
        interpreterPath
    };
    rememberLiveDebugConfig(context, folder, config);
    await vscode.debug.startDebugging(folder, config);
}

class AtomicProofTraceDebugConfigurationProvider
{
    resolveDebugConfiguration(_folder, config)
    {
        if (!config.type) {
            config.type = TRACE_DEBUG_TYPE;
        }
        if (!config.request) {
            config.request = "launch";
        }
        if (!config.name) {
            config.name = "Debug AtomicProof Stack Trace";
        }
        if (!config.tracePath) {
            const active = vscode.window.activeTextEditor?.document.uri;
            if (active?.scheme === "file") {
                config.tracePath = active.fsPath;
            }
        } else if (config.tracePath === "${file}") {
            const active = vscode.window.activeTextEditor?.document.uri;
            if (active?.scheme === "file") {
                config.tracePath = active.fsPath;
            }
        }
        return config;
    }
}

class AtomicProofTraceDebugAdapterFactory
{
    createDebugAdapterDescriptor()
    {
        return new vscode.DebugAdapterInlineImplementation(
            new AtomicProofTraceDebugAdapter()
        );
    }
}

class AtomicProofLiveDebugConfigurationProvider
{
    resolveDebugConfiguration(folder, config)
    {
        if (!config.type) {
            config.type = LIVE_DEBUG_TYPE;
        }
        if (!config.request) {
            config.request = "launch";
        }
        if (!config.name) {
            config.name = "Live Debug AtomicProof Contract";
        }

        const active = vscode.window.activeTextEditor?.document.uri;
        if (!config.contractPath || config.contractPath === "${file}") {
            if (isContractUri(active)) {
                config.contractPath = active.fsPath;
            }
        }

        const workspaceRoot = folder?.uri.fsPath || getWorkspaceRoot() ||
            (config.contractPath ? path.dirname(config.contractPath) : "");
        if (!config.interpreterPath && workspaceRoot) {
            config.interpreterPath = getInterpreterPath(
                workspaceRoot,
                config.contractPath || ""
            );
        }

        if (typeof config.arguments === "string") {
            config.arguments = splitArgs(config.arguments);
        }
        if (!Array.isArray(config.arguments)) {
            config.arguments = [];
        }
        if (!config.functionName) {
            config.functionName =
                String(getConfig().get("defaultFunction") || "").trim();
        }
        return config;
    }
}

class AtomicProofLiveDebugAdapterFactory
{
    createDebugAdapterDescriptor()
    {
        return new vscode.DebugAdapterInlineImplementation(
            new AtomicProofLiveDebugAdapter()
        );
    }
}

class AtomicProofDebugAdapterBase
{
    constructor()
    {
        this.emitter = new vscode.EventEmitter();
        this.onDidSendMessage = this.emitter.event;
        this.seq = 1;
        this.variableRefs = new Map();
        this.nextVarRef = 1;
    }

    dispose()
    {
        this.emitter.dispose();
    }

    createVariableRef(variables)
    {
        const ref = this.nextVarRef++;
        this.variableRefs.set(ref, variables);
        return ref;
    }

    sendResponse(request, body)
    {
        this.emitter.fire({
            type: "response",
            seq: this.seq++,
            request_seq: request.seq,
            success: true,
            command: request.command,
            body: body || {}
        });
    }

    sendErrorResponse(request, error)
    {
        this.emitter.fire({
            type: "response",
            seq: this.seq++,
            request_seq: request.seq,
            success: false,
            command: request.command,
            message: error.message
        });
    }

    sendEvent(event, body)
    {
        this.emitter.fire({
            type: "event",
            seq: this.seq++,
            event,
            body: body || {}
        });
    }

    sendStopped(reason, details = {})
    {
        this.sendEvent("stopped", {
            reason,
            threadId: 1,
            allThreadsStopped: true,
            ...details
        });
    }
}

class AtomicProofLiveDebugAdapter extends AtomicProofDebugAdapterBase
{
    constructor()
    {
        super();
        this.protocolSeq = 1;
        this.pending = new Map();
        this.stdoutBuffer = "";
        this.child = null;
        this.currentSnapshot = null;
        this.contractPath = "";
        this.workspaceRoot = "";
        this.readyPromise = null;
        this.resolveReady = null;
        this.rejectReady = null;
        this.terminated = false;
        this.liveFrames = new Map();
        this.currentFrameId = 0;
        this.lastException = null;
        this.executionState = "created";
        this.entryStoppedSent = false;
        this.shutdownRequested = false;
        this.protocolEventDeferralCount = 0;
        this.deferredProtocolEvents = [];
    }

    dispose()
    {
        this.shutdownRequested = true;
        this.stopChild();
        super.dispose();
    }

    handleMessage(message)
    {
        if (!message || message.type !== "request") {
            return;
        }
        this.handleMessageAsync(message).catch((error) => {
            this.sendErrorResponse(message, error);
        });
    }

    async handleMessageAsync(message)
    {
        switch (message.command) {
        case "initialize":
            this.handleInitialize(message);
            break;
        case "launch":
            await this.handleLaunch(message);
            break;
        case "setBreakpoints":
            await this.handleSetBreakpoints(message);
            break;
        case "configurationDone":
            this.sendResponse(message);
            if (!this.entryStoppedSent && !this.terminated) {
                this.entryStoppedSent = true;
                this.executionState = "paused";
                this.sendStopped("entry");
            }
            break;
        case "threads":
            this.sendResponse(message, {
                threads: [{id: 1, name: "AtomicProof Live VM"}]
            });
            break;
        case "stackTrace":
            await this.handleStackTrace(message);
            break;
        case "scopes":
            this.handleScopes(message);
            break;
        case "variables":
            await this.handleVariables(message);
            break;
        case "evaluate":
            await this.handleEvaluate(message);
            break;
        case "next":
        case "stepIn":
        case "stepOut":
        case "continue":
        case "pause":
            await this.handleExecutionCommand(message);
            break;
        case "source":
            this.handleSource(message);
            break;
        case "exceptionInfo":
            this.handleExceptionInfo(message);
            break;
        case "terminate":
            await this.handleTerminate(message);
            break;
        case "disconnect":
            await this.handleDisconnect(message);
            break;
        default:
            this.sendResponse(message);
            break;
        }
    }

    handleInitialize(message)
    {
        this.sendResponse(message, {
            supportsConfigurationDoneRequest: true,
            supportsEvaluateForHovers: true,
            supportsSetVariable: false,
            supportsStepBack: false,
            supportsTerminateRequest: true,
            supportsExceptionInfoRequest: true,
            supportsConditionalBreakpoints: false,
            supportsHitConditionalBreakpoints: false
        });
    }

    async handleLaunch(message)
    {
        const args = message.arguments || {};
        if (this.child) {
            throw new Error("A Live VM debug server is already running.");
        }
        const contractPath = resolveLiveContractPath(String(args.contractPath || ""));
        if (!contractPath) {
            throw new Error("launch configuration is missing contractPath");
        }

        this.contractPath = contractPath;
        this.workspaceRoot = getWorkspaceRoot() || path.dirname(contractPath);
        this.executionState = "starting";
        this.terminated = false;
        this.entryStoppedSent = false;
        this.shutdownRequested = false;
        this.lastException = null;
        this.currentSnapshot = null;
        this.variableRefs.clear();
        this.liveFrames.clear();
        const interpreterPath = resolveLiveInterpreterPath(
            args.interpreterPath,
            this.workspaceRoot,
            contractPath
        );

        if (!fs.existsSync(interpreterPath)) {
            throw new Error(
                `Executable not found: ${interpreterPath}. Build the project first or set ${CONFIG_SECTION}.interpreterPath.`
            );
        }

        const protocolArgs = [
            "debug-server",
            contractPath
        ];
        const functionName = String(args.functionName || "").trim();
        const debugArguments = normalizeDebugArguments(args.arguments);
        if (!functionName && debugArguments.length > 0) {
            throw new Error(
                "functionName is required when live debug arguments are provided."
            );
        }
        if (functionName) {
            protocolArgs.push(functionName);
        }
        protocolArgs.push(...debugArguments);
        if (args.txFile) {
            protocolArgs.push("--txfile", resolveConfiguredPath(
                String(args.txFile),
                this.workspaceRoot,
                contractPath
            ));
        }

        this.startChild(interpreterPath, protocolArgs);
        await this.waitForReady();
        await this.sendProtocolRequest("initialize");
        this.executionState = "paused";
        this.sendResponse(message);
        this.sendEvent("initialized");
    }

    async handleSetBreakpoints(message)
    {
        const requested = message.arguments?.breakpoints || [];
        const source = message.arguments?.source || {};
        if (!this.child) {
            this.sendResponse(message, {
                breakpoints: requested.map((bp, index) => ({
                    id: index + 1,
                    verified: false,
                    line: bp.line,
                    message: "Live VM debug server has not started."
                }))
            });
            return;
        }

        const response = await this.sendProtocolRequest("setBreakpoints", {
            source,
            breakpoints: requested.map((bp) => ({
                line: Number(bp.line || 0)
            }))
        });
        const breakpoints = response.breakpoints || [];
        this.sendResponse(message, {
            breakpoints: breakpoints.map((bp, index) => ({
                id: Number(bp.id || index + 1),
                verified: Boolean(bp.verified),
                line: Number(bp.line || requested[index]?.line || 0),
                message: bp.message
            }))
        });
    }

    async handleStackTrace(message)
    {
        const response = this.child
            ? await this.sendProtocolRequest("stackTrace")
            : {frames: []};
        const protocolFrames = Array.isArray(response.frames)
            ? response.frames
            : [];
        this.liveFrames.clear();
        this.currentFrameId = 0;
        const stackFrames = protocolFrames.map((frame, index) => {
            const source = frame.source || {};
            const sourceFile = source.file || this.contractPath;
            const sourcePath = resolveLiveSourcePath(this.contractPath, sourceFile);
            const id = Number(frame.id || index + 1);
            this.liveFrames.set(id, frame);
            if (index === 0) {
                this.currentFrameId = id;
            }
            const line = Number(source.line || 0);
            const column = Number(source.column || 0);
            return {
                id,
                name: frame.name || "Live VM",
                source: sourcePath && line > 0 ? {
                    name: path.basename(sourcePath),
                    path: sourcePath
                } : undefined,
                line: line > 0 ? line : 1,
                column: column > 0 ? column : 1
            };
        });
        this.sendResponse(message, {
            stackFrames,
            totalFrames: stackFrames.length
        });
    }

    handleScopes(message)
    {
        const frameId = Number(message.arguments?.frameId || this.currentFrameId || 0);
        this.sendResponse(message, {
            scopes: [
                {
                    name: "Locals",
                    variablesReference: this.createVariableRef({
                        remote: true,
                        scope: "locals",
                        frameId
                    }),
                    expensive: false
                },
                {
                    name: "Globals",
                    variablesReference: this.createVariableRef({
                        remote: true,
                        scope: "globals",
                        frameId
                    }),
                    expensive: false
                },
                {
                    name: "Instruction",
                    variablesReference: this.createVariableRef({
                        remote: true,
                        scope: "instruction",
                        frameId
                    }),
                    expensive: false
                },
                {
                    name: "Main Stack",
                    variablesReference: this.createVariableRef({
                        remote: true,
                        scope: "mainStack",
                        frameId
                    }),
                    expensive: false
                },
                {
                    name: "Alt Stack",
                    variablesReference: this.createVariableRef({
                        remote: true,
                        scope: "altStack",
                        frameId
                    }),
                    expensive: false
                },
                {
                    name: "Call Stack",
                    variablesReference: this.createVariableRef(
                        liveCallStackVariables(this.currentSnapshot?.callStack)
                    ),
                    expensive: false
                },
                {
                    name: "Warnings",
                    variablesReference: this.createVariableRef(
                        liveWarningVariables(this.currentSnapshot?.warnings)
                    ),
                    expensive: false
                },
                {
                    name: "Errors",
                    variablesReference: this.createVariableRef(
                        liveErrorVariables(this.currentSnapshot || {})
                    ),
                    expensive: false
                }
            ]
        });
    }

    async handleVariables(message)
    {
        const ref = Number(message.arguments?.variablesReference || 0);
        const descriptor = this.variableRefs.get(ref);
        if (descriptor?.remote) {
            const response = await this.sendProtocolRequest("variables", {
                scope: descriptor.scope,
                frameId: descriptor.frameId
            });
            this.sendResponse(message, {
                variables: liveProtocolVariables(this, response.variables || [])
            });
            return;
        }
        this.sendResponse(message, {
            variables: Array.isArray(descriptor) ? descriptor : []
        });
    }

    async handleEvaluate(message)
    {
        const expression = String(message.arguments?.expression || "").trim();
        const response = this.child
            ? await this.sendProtocolRequest("evaluate", {
                expression,
                frameId: Number(
                    message.arguments?.frameId || this.currentFrameId || 0
                )
            })
            : evaluateLiveExpression(this, expression);
        const result = response.result !== undefined
            ? String(response.result)
            : String(response.value ?? "");
        this.sendResponse(message, {
            result,
            variablesReference: liveEvaluateVariableReference(this, response)
        });
    }

    async handleExecutionCommand(message)
    {
        const command = message.command;
        if (command === "pause") {
            if (this.executionState !== "running") {
                throw new Error("AtomicProof VM is not running.");
            }
        } else if (this.executionState === "running") {
            throw new Error("AtomicProof VM is already running.");
        } else if (this.executionState === "error") {
            throw new Error(
                "AtomicProof VM stopped on a runtime error; restart the debug session."
            );
        } else if (this.terminated) {
            throw new Error("AtomicProof VM debug session has terminated.");
        }

        const previousState = this.executionState;
        if (command !== "pause") {
            this.executionState = "running";
            this.variableRefs.clear();
            this.liveFrames.clear();
            this.currentFrameId = 0;
        }

        let response;
        this.beginProtocolEventDeferral();
        try {
            response = await this.sendProtocolRequest(command);
        } catch (error) {
            this.executionState = previousState;
            this.endProtocolEventDeferral();
            throw error;
        }
        this.sendResponse(message, command === "continue"
            ? {allThreadsContinued: Boolean(response.allThreadsContinued)}
            : {});
        this.endProtocolEventDeferral();
    }

    handleSource(message)
    {
        const sourcePath = message.arguments?.source?.path || this.contractPath;
        if (sourcePath && fs.existsSync(sourcePath)) {
            this.sendResponse(message, {
                content: fs.readFileSync(sourcePath, "utf8"),
                mimeType: "text/plain"
            });
            return;
        }
        this.sendResponse(message, {content: "", mimeType: "text/plain"});
    }

    handleExceptionInfo(message)
    {
        const error = this.lastException || this.currentSnapshot?.error ||
            "AtomicProof runtime error";
        this.sendResponse(message, {
            exceptionId: "AtomicProofRuntimeError",
            description: error,
            breakMode: "always",
            details: {
                message: error,
                typeName: "RuntimeError"
            }
        });
    }

    async handleDisconnect(message)
    {
        this.shutdownRequested = true;
        this.beginProtocolEventDeferral();
        if (this.child) {
            try {
                await this.sendProtocolRequest("disconnect");
            } catch (_error) {
                this.stopChild();
            }
        }
        this.sendResponse(message);
        this.endProtocolEventDeferral();
    }

    async handleTerminate(message)
    {
        this.shutdownRequested = true;
        this.beginProtocolEventDeferral();
        const child = this.child;
        if (this.child) {
            try {
                await this.sendProtocolRequest("terminate");
            } catch (_error) {
                this.stopChild();
            }
        }
        if (child && this.child === child) {
            await this.waitForChildExit(child);
        }
        this.sendResponse(message);
        this.endProtocolEventDeferral();
        if (!this.terminated) {
            this.terminated = true;
            this.executionState = "terminated";
            this.rejectPending(new Error("Live VM debug session terminated."));
            this.sendEvent("terminated");
        }
    }

    startChild(interpreterPath, args)
    {
        output.appendLine(formatCommand(interpreterPath, args));
        this.readyPromise = new Promise((resolve, reject) => {
            this.resolveReady = resolve;
            this.rejectReady = reject;
        });
        this.child = cp.spawn(interpreterPath, args, {
            cwd: this.workspaceRoot || path.dirname(this.contractPath)
        });
        this.child.stdout.on("data", (chunk) => this.handleStdout(chunk));
        this.child.stderr.on("data", (chunk) => {
            const text = chunk.toString();
            output.append(text);
            this.sendEvent("output", {
                category: "stderr",
                output: text
            });
        });
        this.child.on("error", (error) => {
            this.rejectPending(error);
            this.rejectReady?.(error);
            this.sendEvent("output", {
                category: "stderr",
                output: error.message + "\n"
            });
        });
        this.child.on("close", (code, signal) => {
            this.child = null;
            const exitError = new Error("Live VM debug server exited.");
            this.rejectPending(exitError);
            this.rejectReady?.(exitError);
            if (!this.terminated) {
                const detail = signal
                    ? `signal ${signal}`
                    : `code ${code}`;
                if (!this.shutdownRequested) {
                    this.sendEvent("output", {
                        category: "console",
                        output: `AtomicProof live debug server exited with ${detail}.\n`
                    });
                }
                if (this.protocolEventDeferralCount > 0) {
                    this.deferredProtocolEvents.push({
                        type: "event",
                        event: "terminated",
                        body: {}
                    });
                } else {
                    this.terminated = true;
                    this.executionState = "terminated";
                    this.sendEvent("terminated");
                }
            }
        });
    }

    handleStdout(chunk)
    {
        this.stdoutBuffer += chunk.toString();
        let newline;
        while ((newline = this.stdoutBuffer.indexOf("\n")) !== -1) {
            const line = this.stdoutBuffer.slice(0, newline).trim();
            this.stdoutBuffer = this.stdoutBuffer.slice(newline + 1);
            if (line) {
                this.handleProtocolLine(line);
            }
        }
    }

    handleProtocolLine(line)
    {
        let message;
        try {
            message = JSON.parse(line);
        } catch (_error) {
            output.appendLine("Invalid live debug protocol line: " + line);
            return;
        }

        if (message.type === "response") {
            const pending = this.pending.get(Number(message.request_seq));
            if (pending) {
                this.pending.delete(Number(message.request_seq));
                if (message.success) {
                    if (message.body?.snapshot) {
                        this.currentSnapshot = message.body.snapshot;
                    }
                    pending.resolve(message.body || {});
                } else {
                    pending.reject(new Error(message.message || "Live VM request failed"));
                }
            }
            return;
        }

        if (message.type === "event") {
            this.handleProtocolEvent(message);
        }
    }

    handleProtocolEvent(message)
    {
        if (this.protocolEventDeferralCount > 0 &&
            ["continued", "stopped", "terminated"].includes(message.event)) {
            this.deferredProtocolEvents.push(message);
            return;
        }
        const body = message.body || {};
        if (this.terminated && message.event !== "ready") {
            return;
        }
        if (body.snapshot) {
            this.currentSnapshot = body.snapshot;
        }
        if (message.event === "ready") {
            this.resolveReady?.();
            this.resolveReady = null;
            this.rejectReady = null;
            return;
        }
        if (message.event === "stopped") {
            this.variableRefs.clear();
            this.nextVarRef = 1;
            this.liveFrames.clear();
            this.currentFrameId = 0;
            const reason = body.reason === "error" || body.reason === "exception"
                ? "exception"
                : body.reason || "step";
            this.executionState = reason === "exception" ? "error" : "paused";
            const error = body.description || body.text || body.snapshot?.error;
            if (reason === "exception" && error) {
                this.lastException = String(error);
                this.sendEvent("output", {
                    category: "stderr",
                    output: this.lastException + "\n"
                });
                this.sendStopped(reason, {
                    description: this.lastException,
                    text: this.lastException
                });
            } else {
                this.sendStopped(reason);
            }
            return;
        }
        if (message.event === "continued") {
            this.executionState = "running";
            this.sendEvent("continued", {
                threadId: Number(body.threadId || 1),
                allThreadsContinued: body.allThreadsContinued !== false
            });
            return;
        }
        if (message.event === "terminated") {
            if (!this.terminated) {
                this.terminated = true;
                this.executionState = "terminated";
                this.sendEvent("terminated");
            }
            return;
        }
        if (message.event === "error") {
            const error = new Error(String(
                body.message || "Live VM debug server error"
            ));
            this.lastException = error.message;
            this.rejectReady?.(error);
            this.sendEvent("output", {
                category: "stderr",
                output: error.message + "\n"
            });
        }
    }

    flushDeferredProtocolEvents()
    {
        const events = this.deferredProtocolEvents.splice(0);
        for (const event of events) {
            this.handleProtocolEvent(event);
        }
    }

    beginProtocolEventDeferral()
    {
        this.protocolEventDeferralCount += 1;
    }

    endProtocolEventDeferral()
    {
        this.protocolEventDeferralCount = Math.max(
            0,
            this.protocolEventDeferralCount - 1
        );
        if (this.protocolEventDeferralCount === 0) {
            this.flushDeferredProtocolEvents();
        }
    }

    waitForReady()
    {
        return this.readyPromise || Promise.resolve();
    }

    waitForChildExit(child, timeoutMs = 1000)
    {
        if (!child || child.exitCode !== null || child.signalCode !== null) {
            return Promise.resolve();
        }
        return new Promise((resolve) => {
            let settled = false;
            const finish = () => {
                if (settled) return;
                settled = true;
                clearTimeout(timer);
                resolve();
            };
            const timer = setTimeout(() => {
                if (this.child === child) {
                    this.stopChild();
                }
                finish();
            }, timeoutMs);
            child.once("close", finish);
        });
    }

    sendProtocolRequest(command, args = {})
    {
        if (!this.child || !this.child.stdin.writable) {
            return Promise.reject(new Error("Live VM debug server is not running."));
        }
        const seq = this.protocolSeq++;
        const payload = Object.assign({seq, command}, args);
        return new Promise((resolve, reject) => {
            this.pending.set(seq, {resolve, reject});
            this.child.stdin.write(JSON.stringify(payload) + "\n", (error) => {
                if (error) {
                    this.pending.delete(seq);
                    reject(error);
                }
            });
        });
    }

    rejectPending(error)
    {
        for (const pending of this.pending.values()) {
            pending.reject(error);
        }
        this.pending.clear();
    }

    stopChild()
    {
        if (this.child) {
            this.child.kill();
            this.child = null;
        }
    }

}

class AtomicProofTraceDebugAdapter extends AtomicProofDebugAdapterBase
{
    constructor()
    {
        super();
        this.trace = null;
        this.tracePath = "";
        this.current = 0;
        this.breakpoints = [];
    }

    handleMessage(message)
    {
        if (!message || message.type !== "request") {
            return;
        }

        try {
            switch (message.command) {
            case "initialize":
                this.handleInitialize(message);
                break;
            case "launch":
                this.handleLaunch(message);
                break;
            case "setBreakpoints":
                this.handleSetBreakpoints(message);
                break;
            case "configurationDone":
                this.sendResponse(message);
                this.sendStopped("entry");
                break;
            case "threads":
                this.sendResponse(message, {
                    threads: [{id: 1, name: "AtomicProof Trace"}]
                });
                break;
            case "stackTrace":
                this.handleStackTrace(message);
                break;
            case "scopes":
                this.handleScopes(message);
                break;
            case "variables":
                this.handleVariables(message);
                break;
            case "evaluate":
                this.handleEvaluate(message);
                break;
            case "next":
            case "stepIn":
            case "stepOut":
                this.handleNext(message);
                break;
            case "stepBack":
                this.handleStepBack(message);
                break;
            case "continue":
                this.handleContinue(message);
                break;
            case "reverseContinue":
                this.handleReverseContinue(message);
                break;
            case "pause":
                this.sendResponse(message);
                this.sendStopped("pause");
                break;
            case "source":
                this.handleSource(message);
                break;
            case "disconnect":
                this.sendResponse(message);
                this.sendEvent("terminated");
                break;
            default:
                this.sendResponse(message);
                break;
            }
        } catch (error) {
            this.sendErrorResponse(message, error);
        }
    }

    handleInitialize(message)
    {
        this.sendResponse(message, {
            supportsConfigurationDoneRequest: true,
            supportsStepInTargetsRequest: false,
            supportsEvaluateForHovers: true,
            supportsSetVariable: false,
            supportsStepBack: true,
            supportsConditionalBreakpoints: true,
            supportsHitConditionalBreakpoints: true
        });
    }

    handleLaunch(message)
    {
        const tracePath = resolveDebugTracePath(String(message.arguments?.tracePath || ""));
        if (!tracePath) {
            throw new Error("launch configuration is missing tracePath");
        }
        const trace = JSON.parse(fs.readFileSync(tracePath, "utf8"));
        validateTrace(trace);
        this.trace = trace;
        this.tracePath = tracePath;
        this.current = 0;
        this.sendResponse(message);
        this.sendEvent("initialized");
    }

    handleSetBreakpoints(message)
    {
        const sourcePath = message.arguments?.source?.path || "";
        const defaultSource = sourceFileForTraceStep(
            this.trace,
            this.steps()[0],
            this.tracePath
        );
        const mappedSourcePaths = [...new Set(this.steps().map((step) =>
            normalizeDebugSourcePath(resolveDebugSourcePath(
                this.tracePath,
                sourceFileForTraceStep(this.trace, step, this.tracePath)
            ))
        ))];
        const basenameMatches = sourcePath && !path.isAbsolute(sourcePath)
            ? mappedSourcePaths.filter((candidate) =>
                path.basename(candidate) === path.basename(sourcePath)
            )
            : [];
        const normalizedSourcePath = basenameMatches.length === 1
            ? basenameMatches[0]
            : normalizeDebugSourcePath(sourcePath
                ? resolveDebugSourcePath(this.tracePath, sourcePath)
                : resolveDebugSourcePath(this.tracePath, defaultSource));
        const hasMappedStep = (line) => this.steps().some((step) =>
            sourceLineForTraceStep(step) === line &&
            normalizeDebugSourcePath(resolveDebugSourcePath(
                this.tracePath,
                sourceFileForTraceStep(this.trace, step, this.tracePath)
            )) === normalizedSourcePath
        );
        const requested = message.arguments?.breakpoints || [];
        this.breakpoints = requested.map((bp, index) => ({
            id: index + 1,
            path: sourcePath,
            normalizedPath: normalizedSourcePath,
            line: Number(bp.line || 0),
            condition: String(bp.condition || "").trim(),
            hitCondition: String(bp.hitCondition || "").trim(),
            logMessage: String(bp.logMessage || "").trim(),
            hitCount: 0
        }));
        this.sendResponse(message, {
            breakpoints: this.breakpoints.map((bp) => ({
                id: bp.id,
                verified: hasMappedStep(bp.line),
                line: bp.line,
                message: hasMappedStep(bp.line)
                    ? undefined
                    : "No trace step is mapped to this source line."
            }))
        });
    }

    handleStackTrace(message)
    {
        const step = this.currentStep();
        const sourceFile = sourceFileForTraceStep(this.trace, step, this.tracePath);
        const sourcePath = resolveDebugSourcePath(this.tracePath, sourceFile);
        this.sendResponse(message, {
            stackFrames: [{
                id: this.current + 1,
                name: step
                    ? (step.opcode || step.instruction || "instruction")
                    : "No trace loaded",
                source: sourcePath ? {
                    name: path.basename(sourcePath),
                    path: sourcePath
                } : undefined,
                line: Math.max(1, sourceLineForTraceStep(step) || 1),
                column: Math.max(1, sourceColumnForTraceStep(step) || 1)
            }],
            totalFrames: step ? 1 : 0
        });
    }

    handleScopes(message)
    {
        const step = this.currentStep();
        this.variableRefs.clear();
        this.nextVarRef = 1;
        this.sendResponse(message, {
            scopes: [
                {
                    name: "Instruction",
                    variablesReference: this.createVariableRef(instructionVariables(step)),
                    expensive: false
                },
                {
                    name: "Main Stack After",
                    variablesReference: this.createVariableRef(stackVariables(step, "main")),
                    expensive: false
                },
                {
                    name: "Alt Stack After",
                    variablesReference: this.createVariableRef(stackVariables(step, "alt")),
                    expensive: false
                },
                {
                    name: "Effects",
                    variablesReference: this.createVariableRef(effectVariables(step)),
                    expensive: false
                }
            ]
        });
    }

    handleVariables(message)
    {
        const ref = Number(message.arguments?.variablesReference || 0);
        this.sendResponse(message, {
            variables: this.variableRefs.get(ref) || []
        });
    }

    handleEvaluate(message)
    {
        const expression = String(message.arguments?.expression || "").trim();
        const result = evaluateTraceExpression(this, expression);
        this.sendResponse(message, {
            result: result.value,
            variablesReference: result.variablesReference || 0
        });
    }

    handleNext(message)
    {
        if (this.current < this.steps().length - 1) {
            this.current += 1;
            this.sendResponse(message);
            this.sendStopped("step");
        } else {
            this.sendResponse(message);
            this.sendStopped("end");
        }
    }

    handleStepBack(message)
    {
        if (this.current > 0) {
            this.current -= 1;
            this.sendResponse(message);
            this.sendStopped("step");
        } else {
            this.sendResponse(message);
            this.sendStopped("entry");
        }
    }

    handleContinue(message)
    {
        let reason = "end";
        for (let index = this.current + 1; index < this.steps().length; index++) {
            this.current = index;
            if (this.isBreakpointStep(this.steps()[index])) {
                reason = "breakpoint";
                break;
            }
        }
        this.sendResponse(message, {allThreadsContinued: false});
        this.sendStopped(reason);
    }

    handleReverseContinue(message)
    {
        let reason = "entry";
        for (let index = this.current - 1; index >= 0; index--) {
            this.current = index;
            if (this.isBreakpointStep(this.steps()[index])) {
                reason = "breakpoint";
                break;
            }
        }
        this.sendResponse(message, {allThreadsContinued: false});
        this.sendStopped(reason);
    }

    handleSource(message)
    {
        const sourcePath = message.arguments?.source?.path || "";
        const embedded = this.trace?.source?.lines;
        if (Array.isArray(embedded)) {
            this.sendResponse(message, {
                content: embedded.join("\n"),
                mimeType: "text/plain"
            });
            return;
        }
        if (sourcePath && fs.existsSync(sourcePath)) {
            this.sendResponse(message, {
                content: fs.readFileSync(sourcePath, "utf8"),
                mimeType: "text/plain"
            });
            return;
        }
        this.sendResponse(message, {content: "", mimeType: "text/plain"});
    }

    isBreakpointStep(step)
    {
        const line = sourceLineForTraceStep(step);
        if (!line) {
            return false;
        }
        const currentPath = normalizeDebugSourcePath(resolveDebugSourcePath(
            this.tracePath,
            sourceFileForTraceStep(this.trace, step, this.tracePath)
        ));
        for (const breakpoint of this.breakpoints) {
            if (breakpoint.line !== line ||
                breakpoint.normalizedPath !== currentPath) {
                continue;
            }
            breakpoint.hitCount += 1;
            if (!hitConditionMatches(breakpoint.hitCondition, breakpoint.hitCount)) {
                continue;
            }
            if (breakpoint.condition &&
                !evaluateTraceCondition(this, breakpoint.condition)) {
                continue;
            }
            if (breakpoint.logMessage) {
                this.sendEvent("output", {
                    category: "console",
                    output: formatLogpointMessage(this, breakpoint.logMessage) + "\n"
                });
                continue;
            }
            return true;
        }
        return false;
    }

    steps()
    {
        return this.trace && Array.isArray(this.trace.steps)
            ? this.trace.steps
            : [];
    }

    currentStep()
    {
        return this.steps()[this.current] || null;
    }
}

function instructionVariables(step)
{
    if (!step) {
        return [];
    }
    return [
        dapVariable("step", step.step ?? ""),
        dapVariable("pc", step.pc ?? ""),
        dapVariable("opcode", step.opcode || step.instruction || ""),
        dapVariable("operand", step.operand || ""),
        dapVariable("function", step.functionName || ""),
        dapVariable("source", `${sourceFileForTraceStep(null, step, "")}:${sourceLineForTraceStep(step) || ""}`)
    ];
}

function stackVariables(step, stackName)
{
    return stackVariablesFromValues(stackForTrace(step, stackName, "after"));
}

function stackVariablesFromValues(values)
{
    const stack = topFirstTrace(values || []);
    const visible = stack.slice(0, 120).map((item, index) => ({
        name: index === 0 ? "[0] top" : `[${index}]`,
        value: valueSummaryForTrace(item),
        variablesReference: 0
    }));
    if (stack.length > visible.length) {
        visible.push(dapVariable(
            "hidden",
            `${stack.length - visible.length} additional value(s) omitted`
        ));
    }
    return visible;
}

function effectVariables(step)
{
    if (!step) {
        return [];
    }
    const effects = step.effects || {};
    const main = effects.mainStack || {};
    const alt = effects.altStack || {};
    return [
        dapVariable("main pushed", countTrace(main.pushed)),
        dapVariable("main popped", countTrace(main.popped)),
        dapVariable("alt pushed", countTrace(alt.pushed)),
        dapVariable("alt popped", countTrace(alt.popped)),
        dapVariable("moves", countTrace(effects.moves)),
        dapVariable(
            "move summary",
            (effects.moves || []).map((move) =>
                `${valueSummaryForTrace(move.element)} ${move.from || "-"} -> ${move.to || "-"}`
            ).join(", ")
        )
    ];
}

function evaluateTraceExpression(adapter, expression)
{
    if (!expression) {
        return {value: ""};
    }
    const value = evaluateTraceValue(adapter, expression);
    if (Array.isArray(value)) {
        return {
            value: `Array(${value.length})`,
            variablesReference: adapter.createVariableRef(
                value.slice(0, 120).map((item, index) => ({
                    name: `[${index}]`,
                    value: valueSummaryForTrace(item),
                    variablesReference: 0
                }))
            )
        };
    }
    if (value && typeof value === "object") {
        return {value: JSON.stringify(value)};
    }
    return {value: String(value ?? "")};
}

function evaluateTraceCondition(adapter, expression)
{
    const comparison = expression.match(/^(.+?)\s*(==|!=|>=|<=|>|<)\s*(.+)$/);
    if (!comparison) {
        return Boolean(evaluateTraceValue(adapter, expression));
    }
    const left = evaluateTraceValue(adapter, comparison[1].trim());
    const right = literalOrTraceValue(adapter, comparison[3].trim());
    switch (comparison[2]) {
    case "==":
        return String(left) === String(right);
    case "!=":
        return String(left) !== String(right);
    case ">=":
        return Number(left) >= Number(right);
    case "<=":
        return Number(left) <= Number(right);
    case ">":
        return Number(left) > Number(right);
    case "<":
        return Number(left) < Number(right);
    default:
        return false;
    }
}

function evaluateTraceValue(adapter, expression)
{
    const step = adapter.currentStep();
    const trimmed = String(expression || "").trim();
    if (!step) {
        return "";
    }

    const main = topFirstTrace(stackForTrace(step, "main", "after"));
    const alt = topFirstTrace(stackForTrace(step, "alt", "after"));
    const map = {
        step: adapter.current,
        traceStep: step.step ?? adapter.current,
        pc: step.pc ?? "",
        opcode: step.opcode || step.instruction || "",
        instruction: step.instruction || step.opcode || "",
        operand: step.operand || "",
        function: step.functionName || "",
        functionName: step.functionName || "",
        source: `${sourceFileForTraceStep(adapter.trace, step, adapter.tracePath)}:${sourceLineForTraceStep(step) || ""}`,
        line: sourceLineForTraceStep(step) || 0,
        "main.length": main.length,
        "alt.length": alt.length,
        main,
        alt,
        json: step,
        effects: step.effects || {},
        "effects.moves": (step.effects && step.effects.moves) || []
    };

    if (Object.prototype.hasOwnProperty.call(map, trimmed)) {
        return map[trimmed];
    }

    const stackMatch = trimmed.match(/^(main|alt)\[(\d+)\](?:\.(hex|int|intString|elementId|depth))?$/);
    if (stackMatch) {
        const values = stackMatch[1] === "main" ? main : alt;
        const item = values[Number(stackMatch[2])];
        if (!item) {
            return "";
        }
        return stackMatch[3] ? item[stackMatch[3]] : valueSummaryForTrace(item);
    }

    if (/^-?\d+(\.\d+)?$/.test(trimmed)) {
        return Number(trimmed);
    }
    return unquote(trimmed);
}

function literalOrTraceValue(adapter, expression)
{
    if (/^(['"]).*\1$/.test(expression)) {
        return unquote(expression);
    }
    if (/^-?\d+(\.\d+)?$/.test(expression)) {
        return Number(expression);
    }
    const known = evaluateTraceValue(adapter, expression);
    return known === "" ? expression : known;
}

function unquote(value)
{
    return String(value)
        .replace(/^"(.*)"$/, "$1")
        .replace(/^'(.*)'$/, "$1");
}

function hitConditionMatches(hitCondition, hitCount)
{
    const condition = String(hitCondition || "").replace(/\s+/g, "");
    if (!condition) {
        return true;
    }
    const match = condition.match(/^(>=|<=|>|<|==)?(\d+)$/);
    if (!match) {
        return true;
    }
    const op = match[1] || "==";
    const target = Number(match[2]);
    switch (op) {
    case ">=":
        return hitCount >= target;
    case "<=":
        return hitCount <= target;
    case ">":
        return hitCount > target;
    case "<":
        return hitCount < target;
    case "==":
    default:
        return hitCount === target;
    }
}

function formatLogpointMessage(adapter, message)
{
    return String(message).replace(/\{([^}]+)\}/g, (_match, expression) =>
        String(evaluateTraceValue(adapter, expression.trim()))
    );
}

function dapVariable(name, value)
{
    return {
        name,
        value: String(value ?? ""),
        variablesReference: 0
    };
}

function stackForTrace(step, stackName, phase)
{
    if (!step) {
        return [];
    }
    const prefix = stackName === "main" ? "mainStack" : "altStack";
    const nested = step[prefix];
    if (nested && Array.isArray(nested[phase])) {
        return nested[phase];
    }
    const direct = prefix + (phase === "before" ? "Before" : "After");
    return Array.isArray(step[direct]) ? step[direct] : [];
}

function topFirstTrace(items)
{
    const copy = (items || []).slice();
    if (copy.every((item) => item && Number.isFinite(Number(item.depth)))) {
        return copy.sort((left, right) => Number(left.depth) - Number(right.depth));
    }
    return copy.reverse();
}

function countTrace(items)
{
    return Array.isArray(items) ? items.length : 0;
}

function valueSummaryForTrace(item)
{
    if (!item) {
        return "-";
    }
    const parts = [item.hex || JSON.stringify(item)];
    if (item.intString !== undefined) {
        parts.push(`int=${item.intString}`);
    } else if (item.int !== undefined) {
        parts.push(`int=${item.int}`);
    }
    if (item.elementId) {
        parts.push(`id=${item.elementId}`);
    }
    if (item.depth !== undefined) {
        parts.push(Number(item.depth) === 0 ? "top" : `depth=${item.depth}`);
    }
    return parts.join(" ");
}

function sourceLineForTraceStep(step)
{
    return step ? Number(step.sourceLine || (step.source && step.source.line) || 0) : 0;
}

function sourceColumnForTraceStep(step)
{
    return step ? Number((step.source && step.source.column) || 1) : 1;
}

function sourceFileForTraceStep(trace, step, tracePath)
{
    const source = (trace && trace.source) || {};
    return (step && (step.sourceFile || (step.source && step.source.file))) ||
        source.file ||
        tracePath ||
        "stack_trace.json";
}

function normalizeDebugSourcePath(sourcePath)
{
    if (!sourcePath) {
        return "";
    }
    return normalizeFsPath(canonicalPath(sourcePath));
}

function resolveDebugSourcePath(tracePath, sourceFile)
{
    if (!sourceFile) {
        return "";
    }
    if (path.isAbsolute(sourceFile)) {
        return sourceFile;
    }
    const candidates = sourcePathCandidates(tracePath, sourceFile);
    return candidates.find((candidate) => fs.existsSync(candidate)) || candidates[0] || sourceFile;
}

function resolveDebugTracePath(tracePath)
{
    if (!tracePath || tracePath === "${file}") {
        const active = vscode.window.activeTextEditor?.document.uri;
        return active?.scheme === "file" ? active.fsPath : "";
    }
    if (path.isAbsolute(tracePath)) {
        return tracePath;
    }
    const workspaceRoot = getWorkspaceRoot();
    return workspaceRoot ? path.join(workspaceRoot, tracePath) : tracePath;
}

function resolveLiveContractPath(contractPath)
{
    if (!contractPath || contractPath === "${file}") {
        const active = vscode.window.activeTextEditor?.document.uri;
        return isContractUri(active) ? active.fsPath : "";
    }
    if (path.isAbsolute(contractPath)) {
        return contractPath;
    }
    const workspaceRoot = getWorkspaceRoot();
    return workspaceRoot ? path.join(workspaceRoot, contractPath) : contractPath;
}

function resolveLiveInterpreterPath(interpreterPath, workspaceRoot, contractPath)
{
    const root = workspaceRoot || path.dirname(contractPath);
    const configured = String(interpreterPath || "").trim();
    if (configured) {
        return resolveConfiguredPath(configured, root, contractPath);
    }
    return getInterpreterPath(root, contractPath);
}

function normalizeDebugArguments(value)
{
    if (Array.isArray(value)) {
        return value.map((item) => String(item));
    }
    if (typeof value === "string") {
        return splitArgs(value);
    }
    return [];
}

function resolveLiveSourcePath(contractPath, sourceFile)
{
    if (!sourceFile) {
        return contractPath;
    }
    if (path.isAbsolute(sourceFile)) {
        return sourceFile;
    }
    const candidates = sourcePathCandidates(contractPath, sourceFile);
    return candidates.find((candidate) => fs.existsSync(candidate)) ||
        path.join(path.dirname(contractPath), sourceFile);
}

function liveInstructionVariables(snapshot)
{
    if (!snapshot) {
        return [];
    }
    const source = snapshot.source || {};
    const opcodeSummary = liveLineInstructionSummary(snapshot) ||
        snapshot.opcode || snapshot.instruction || "";
    return [
        dapVariable("pc", snapshot.pc ?? ""),
        dapVariable("state", snapshot.state || ""),
        dapVariable("opcode", opcodeSummary),
        dapVariable("currentOpcode", snapshot.opcode || snapshot.instruction || ""),
        dapVariable("operand", snapshot.operand || ""),
        dapVariable("function", snapshot.functionName || ""),
        dapVariable("source", `${source.file || ""}:${source.line || ""}`),
        dapVariable("executed", snapshot.instructionCount ?? ""),
        dapVariable("range", `${snapshot.range?.startPC ?? 0}..${snapshot.range?.endPC ?? 0}`)
    ];
}

function liveLineInstructionSummary(snapshot)
{
    if (!snapshot) {
        return "";
    }
    if (snapshot.lineInstructionSummary) {
        return String(snapshot.lineInstructionSummary);
    }
    if (!Array.isArray(snapshot.lineInstructions)) {
        return "";
    }
    return snapshot.lineInstructions.map((item) => {
        const marker = item && item.current ? "*" : "";
        const opcode = item?.opcode || item?.instruction || "";
        const operand = item?.operand ? ` ${item.operand}` : "";
        return `pc ${item?.pc ?? ""}${marker}: ${opcode}${operand}`;
    }).join("; ");
}

function liveStackVariables(values)
{
    return stackVariablesFromValues(values);
}

function liveCallStackVariables(frames)
{
    return (frames || []).map((frame, index) => ({
        name: `[${index}]`,
        value: `${frame.functionName || "frame"} returnPC=${frame.returnPC ?? ""}`,
        variablesReference: 0
    }));
}

function liveWarningVariables(warnings)
{
    return (warnings || []).map((warning, index) => ({
        name: `[${index}]`,
        value: String(warning),
        variablesReference: 0
    }));
}

function liveErrorVariables(snapshot)
{
    const error = snapshot?.error || "";
    return error ? [dapVariable("runtimeError", error)] : [];
}

function liveProtocolVariables(adapter, variables)
{
    return (variables || []).map((variable) => {
        const children = Array.isArray(variable.children)
            ? liveProtocolVariables(adapter, variable.children)
            : [];
        return {
            name: String(variable.name || ""),
            value: String(variable.value ?? ""),
            type: variable.type ? String(variable.type) : undefined,
            variablesReference: children.length
                ? adapter.createVariableRef(children)
                : 0,
            presentationHint: variable.available === false
                ? {attributes: ["readOnly"]}
                : undefined
        };
    });
}

function evaluateLiveExpression(adapter, expression)
{
    const snapshot = adapter.currentSnapshot || {};
    const trimmed = String(expression || "").trim();
    if (!trimmed) {
        return {result: ""};
    }

    const map = {
        pc: snapshot.pc ?? "",
        opcode: snapshot.opcode || snapshot.instruction || "",
        instruction: snapshot.instruction || snapshot.opcode || "",
        operand: snapshot.operand || "",
        function: snapshot.functionName || "",
        functionName: snapshot.functionName || "",
        state: snapshot.state || "",
        line: snapshot.source?.line || 0,
        "main.length": (snapshot.mainStack || []).length,
        "alt.length": (snapshot.altStack || []).length,
        json: snapshot
    };

    if (Object.prototype.hasOwnProperty.call(map, trimmed)) {
        const value = map[trimmed];
        return {
            result: typeof value === "object" ? JSON.stringify(value) : String(value),
            value
        };
    }

    const stackMatch = trimmed.match(/^(main|alt)\[(\d+)\](?:\.(hex|int|intString|depth))?$/);
    if (stackMatch) {
        const values = topFirstTrace(
            stackMatch[1] === "main"
                ? snapshot.mainStack || []
                : snapshot.altStack || []
        );
        const item = values[Number(stackMatch[2])];
        if (!item) {
            return {result: ""};
        }
        const field = stackMatch[3];
        const value = field ? item[field] : valueSummaryForTrace(item);
        return {result: String(value ?? ""), value};
    }

    return {result: trimmed, value: trimmed};
}

function liveEvaluateVariableReference(adapter, response)
{
    const value = response?.value;
    if (!value || typeof value !== "object") {
        return 0;
    }
    if (Array.isArray(value)) {
        return adapter.createVariableRef(value.map((item, index) => ({
            name: `[${index}]`,
            value: typeof item === "object" ? JSON.stringify(item) : String(item),
            variablesReference: 0
        })));
    }
    return adapter.createVariableRef(Object.keys(value).slice(0, 120).map((key) => ({
        name: key,
        value: typeof value[key] === "object"
            ? JSON.stringify(value[key])
            : String(value[key]),
        variablesReference: 0
    })));
}

module.exports = {
    activate,
    deactivate,
    __test: {
        AtomicProofLiveDebugAdapter,
        AtomicProofTraceDebugAdapter,
        buildWebviewHtml,
        evaluateLiveExpression,
        evaluateTraceExpression,
        liveInstructionVariables,
        resolveConfiguredPath,
        sourcePathCandidates,
        splitArgs,
        validateTrace
    }
};

function createStatusBarItem()
{
    statusBarItem = vscode.window.createStatusBarItem(
        vscode.StatusBarAlignment.Left,
        100
    );
    updateStatusBarItem();
    return statusBarItem;
}

function updateStatusBarItem()
{
    if (!statusBarItem) {
        return;
    }
    if (getConfig().get("autoRunOnSave.showStatus") === false) {
        statusBarItem.hide();
        return;
    }

    if (getConfig().get("autoRunOnSave.enabled") === true) {
        const mode = getAutoRunMode() === "live" ? "Live" : "Trace";
        statusBarItem.text = `$(sync) AtomicProof: Auto ${mode}`;
        statusBarItem.tooltip = "Toggle AtomicProof auto debug on save";
        statusBarItem.command = "atomicProofStackVisualizer.toggleAutoDebugOnSave";
        statusBarItem.show();
        return;
    }

    statusBarItem.text = "$(debug-start) AtomicProof Trace";
    statusBarItem.tooltip = "Generate an AtomicProof stack trace and open the visualizer";
    statusBarItem.command = "atomicProofStackVisualizer.generateTrace";
    statusBarItem.show();
}

function getConfig()
{
    return vscode.workspace.getConfiguration(CONFIG_SECTION);
}

function getOpenColumn()
{
    return getConfig().get("openBeside") === false
        ? vscode.ViewColumn.Active
        : vscode.ViewColumn.Beside;
}

function getWebviewShowOptions(options = {})
{
    const viewColumn = getOpenColumn();
    return options.preserveFocus
        ? {viewColumn, preserveFocus: true}
        : viewColumn;
}

function getWebviewStartupOptions()
{
    return {
        viewMode: getConfig().get("defaultViewMode") || "diff",
        speed: Number(getConfig().get("playbackSpeed") || 1)
    };
}

function getInterpreterPath(workspaceRoot, contractPath)
{
    const configured = String(getConfig().get("interpreterPath") || "").trim();
    if (configured) {
        return resolveConfiguredPath(configured, workspaceRoot, contractPath);
    }
    return path.join(
        workspaceRoot,
        "build",
        "bin",
        process.platform === "win32" ? "utxo_Interpreter.exe" : "utxo_Interpreter"
    );
}

function getDefaultArguments(context)
{
    const configured = String(getConfig().get("defaultArguments") || "");
    if (configured) {
        return configured;
    }
    return context.workspaceState.get(LAST_ARGS_KEY) || "";
}

async function chooseContractUri(workspaceRoot, uri)
{
    if (isContractUri(uri)) {
        return uri;
    }

    const activeUri = vscode.window.activeTextEditor?.document.uri;
    if (isContractUri(activeUri)) {
        return activeUri;
    }

    const contractFiles = await vscode.window.showOpenDialog({
        canSelectFiles: true,
        canSelectFolders: false,
        canSelectMany: false,
        defaultUri: vscode.Uri.file(workspaceRoot),
        filters: {
            "AtomicProof Contract": ["ct"]
        },
        title: "Choose a contract to run"
    });

    return contractFiles && contractFiles.length ? contractFiles[0] : undefined;
}

function isContractUri(uri)
{
    return uri && uri.scheme === "file" &&
        path.extname(uri.fsPath).toLowerCase() === ".ct";
}

function normalizeFsPath(filePath)
{
    const normalized = path.normalize(String(filePath || ""));
    return process.platform === "win32" ? normalized.toLowerCase() : normalized;
}

async function chooseFunctionName(context, contractPath, preferredFunction = "")
{
    const preferred = String(preferredFunction || "").trim();
    const configured = String(getConfig().get("defaultFunction") || "").trim();
    const lastFunction = context.workspaceState.get(LAST_FUNCTION_KEY) || "";
    const discovered = await discoverContractFunctions(contractPath);
    const initial = preferred || configured || lastFunction || discovered[0] || "";

    if (discovered.length) {
        const choices = initial && !discovered.includes(initial)
            ? [initial, ...discovered]
            : discovered;
        const items = [
            ...choices.map((name) => ({
                label: name,
                description: name === initial ? "default" : undefined
            })),
            {
                label: "$(edit) Enter custom function..."
            }
        ];
        const picked = await vscode.window.showQuickPick(items, {
            placeHolder: "Function name to run",
            title: "AtomicProof Function"
        });
        if (!picked) {
            return "";
        }
        if (picked.label.startsWith("$(edit)")) {
            return showFunctionInput(initial);
        }
        return picked.label;
    }

    return showFunctionInput(initial);
}

function showFunctionInput(initial)
{
    return vscode.window.showInputBox({
        prompt: "Function name to run",
        value: initial,
        placeHolder: "test_alt_roundtrip"
    });
}

function placeholderArguments(params)
{
    if (!Array.isArray(params) || params.length === 0) {
        return "5";
    }
    return params.map((param) => placeholderArgument(param)).join(" ");
}

function placeholderArgument(param)
{
    const type = String(param?.type || "").toLowerCase();
    const name = String(param?.name || "value").trim() || "value";
    if (type.includes("bool")) {
        return "true";
    }
    if (type.includes("string")) {
        return `"${name}"`;
    }
    if (type.includes("bytes") || type.includes("hash")) {
        return "0x00";
    }
    return "5";
}

async function discoverContractFunctions(contractPath)
{
    try {
        const text = await fs.promises.readFile(contractPath, "utf8");
        const names = [];
        const seen = new Set();
        const re = /^\s*def\s+([A-Za-z_][A-Za-z0-9_]*)\s*\(/gm;
        let match;
        while ((match = re.exec(text)) !== null) {
            if (!seen.has(match[1])) {
                seen.add(match[1]);
                names.push(match[1]);
            }
        }
        return names;
    } catch (_error) {
        return [];
    }
}

function resolveConfiguredPath(template, workspaceRoot, contractPath)
{
    const contractDir = contractPath ? path.dirname(contractPath) : workspaceRoot;
    const contractBase = contractPath ? path.basename(contractPath) : "";
    const contractBaseNoExt = contractPath
        ? path.basename(contractPath, path.extname(contractPath))
        : "";
    let value = String(template)
        .replaceAll("${workspaceFolder}", workspaceRoot)
        .replaceAll("${file}", contractPath || "")
        .replaceAll("${fileDirname}", contractDir)
        .replaceAll("${fileBasename}", contractBase)
        .replaceAll("${fileBasenameNoExtension}", contractBaseNoExt)
        .replaceAll("${home}", os.homedir());

    if (!path.isAbsolute(value)) {
        value = path.join(workspaceRoot, value);
    }
    return path.normalize(value);
}

async function handleWebviewMessage(context, tracePath, message)
{
    if (!message || message.type !== "openSource") {
        return;
    }

    const sourceFile = String(message.file || "").trim();
    if (!sourceFile) {
        vscode.window.showWarningMessage("No source file is associated with this trace step.");
        return;
    }

    const sourcePath = resolveSourcePath(context, tracePath, sourceFile);
    if (!fs.existsSync(sourcePath)) {
        vscode.window.showWarningMessage(`Source file not found: ${sourceFile}`);
        return;
    }

    if (!isTrustedSourcePath(context, tracePath, sourcePath)) {
        const choice = await vscode.window.showWarningMessage(
            `Trace source is outside the workspace and trace directory: ${sourcePath}`,
            "Open Anyway"
        );
        if (choice !== "Open Anyway") {
            return;
        }
    }

    const line = Math.max(0, Number(message.line || 1) - 1);
    const column = Math.max(0, Number(message.column || 1) - 1);
    const document = await vscode.workspace.openTextDocument(vscode.Uri.file(sourcePath));
    await vscode.window.showTextDocument(document, {
        viewColumn: vscode.ViewColumn.One,
        selection: new vscode.Range(line, column, line, column),
        preserveFocus: false
    });
}

function resolveSourcePath(context, tracePath, sourceFile)
{
    if (path.isAbsolute(sourceFile)) {
        return sourceFile;
    }

    const candidates = sourcePathCandidates(
        tracePath,
        sourceFile,
        [getRepositoryRoot(context)]
    );

    return candidates.find((candidate) => fs.existsSync(candidate)) || candidates[0];
}

function isTrustedSourcePath(context, tracePath, sourcePath)
{
    const roots = [
        ...(vscode.workspace.workspaceFolders || []).map((folder) => folder.uri.fsPath),
        tracePath && path.dirname(tracePath),
        getRepositoryRoot(context)
    ].filter(Boolean);
    const candidate = canonicalPath(sourcePath);
    return roots.some((root) => isPathWithin(canonicalPath(root), candidate));
}

function canonicalPath(filePath)
{
    try {
        return fs.realpathSync.native(filePath);
    } catch (_error) {
        return path.resolve(filePath);
    }
}

function isPathWithin(root, candidate)
{
    const relative = path.relative(root, candidate);
    return relative === "" || (!relative.startsWith(`..${path.sep}`) &&
        relative !== ".." && !path.isAbsolute(relative));
}

function sourcePathCandidates(tracePath, sourceFile, extraRoots = [])
{
    const candidates = [];
    const seen = new Set();
    const pushCandidate = (candidate) => {
        if (!candidate || seen.has(candidate)) {
            return;
        }
        seen.add(candidate);
        candidates.push(candidate);
    };

    pushCandidate(getWorkspaceRoot() && path.join(getWorkspaceRoot(), sourceFile));
    for (const root of extraRoots) {
        pushCandidate(root && path.join(root, sourceFile));
    }

    let current = tracePath ? path.dirname(tracePath) : "";
    while (current) {
        pushCandidate(path.join(current, sourceFile));
        const parent = path.dirname(current);
        if (parent === current) {
            break;
        }
        current = parent;
    }

    return candidates;
}

function formatCommand(file, args)
{
    return "$ " + [file, ...args].map(quoteArg).join(" ");
}

function quoteArg(value)
{
    const text = String(value);
    if (/^[A-Za-z0-9_./:=+@%-]+$/.test(text)) {
        return text;
    }
    return JSON.stringify(text);
}

function getNonce()
{
    const alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    let nonce = "";
    for (let i = 0; i < 32; i++) {
        nonce += alphabet.charAt(Math.floor(Math.random() * alphabet.length));
    }
    return nonce;
}
