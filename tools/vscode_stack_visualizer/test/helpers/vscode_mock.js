"use strict";

const fs = require("fs");
const path = require("path");
const Module = require("module");

class MockEventEmitter
{
    constructor()
    {
        this.listeners = new Set();
        this.event = (listener) => {
            this.listeners.add(listener);
            return {dispose: () => this.listeners.delete(listener)};
        };
    }

    fire(value)
    {
        for (const listener of [...this.listeners]) {
            listener(value);
        }
    }

    dispose()
    {
        this.listeners.clear();
    }
}

class MockUri
{
    constructor(fsPath)
    {
        this.scheme = "file";
        this.fsPath = path.resolve(fsPath);
        this.path = this.fsPath;
    }

    static file(filePath)
    {
        return new MockUri(filePath);
    }
}

function createContext(extensionRoot)
{
    const data = new Map();
    return {
        extensionPath: extensionRoot,
        subscriptions: [],
        workspaceState: {
            get(key, fallback)
            {
                return data.has(key) ? data.get(key) : fallback;
            },
            async update(key, value)
            {
                data.set(key, value);
            }
        },
        __workspaceData: data,
        dispose()
        {
            for (const item of this.subscriptions.splice(0)) {
                item?.dispose?.();
            }
        }
    };
}

function createVscodeMock(options = {})
{
    const workspaceRoots = (options.workspaceRoots || [
        options.workspaceRoot || process.cwd()
    ]).map((root) => path.resolve(root));
    const workspaceRoot = workspaceRoots[0];
    const commandCallbacks = new Map();
    const configuration = new Map(Object.entries(options.configuration || {}));
    const configurationsByRoot = new Map(Object.entries(
        options.configurationByRoot || {}
    ).map(([root, values]) => [
        path.resolve(root),
        new Map(Object.entries(values || {}))
    ]));
    const saveEmitter = new MockEventEmitter();
    const configEmitter = new MockEventEmitter();
    const debugStartEmitter = new MockEventEmitter();
    const debugTerminateEmitter = new MockEventEmitter();
    const state = {
        commandCallbacks,
        configuration,
        errors: [],
        warnings: [],
        infos: [],
        panels: [],
        debugStarts: [],
        debugStops: [],
        output: [],
        status: [],
        openedDocuments: [],
        shownDocuments: [],
        openDialog: [],
        saveDialog: [],
        quickPick: [],
        inputBox: [],
        infoChoice: [],
        errorChoice: [],
        warningChoice: [],
        sessionCounter: 0,
        throwDebugStart: null,
        throwDebugStop: null,
        throwConfigUpdate: null,
        suppressDebugTermination: Boolean(options.suppressDebugTermination)
    };

    const outputChannel = {
        append(value) { state.output.push(String(value)); },
        appendLine(value) { state.output.push(String(value) + "\n"); },
        clear() { state.output.length = 0; },
        show() {},
        dispose() {}
    };

    function queueValue(queue)
    {
        return queue.length ? queue.shift() : undefined;
    }

    const vscode = {
        EventEmitter: MockEventEmitter,
        Uri: MockUri,
        Range: class Range {
            constructor(startLine, startColumn, endLine, endColumn)
            {
                this.start = {line: startLine, character: startColumn};
                this.end = {line: endLine, character: endColumn};
            }
        },
        DebugAdapterInlineImplementation: class DebugAdapterInlineImplementation {
            constructor(implementation) { this.implementation = implementation; }
        },
        StatusBarAlignment: {Left: 1},
        ViewColumn: {Active: 1, Beside: 2, One: 1},
        ConfigurationTarget: {Global: 1, Workspace: 2, WorkspaceFolder: 3},
        ProgressLocation: {Notification: 1, Window: 2},
        window: {
            activeTextEditor: options.activeTextEditor,
            createOutputChannel: () => outputChannel,
            createStatusBarItem: () => ({
                text: "",
                tooltip: "",
                command: "",
                show() {},
                hide() {},
                dispose() {}
            }),
            async showOpenDialog() { return queueValue(state.openDialog); },
            async showSaveDialog() { return queueValue(state.saveDialog); },
            async showQuickPick() { return queueValue(state.quickPick); },
            async showInputBox() { return queueValue(state.inputBox); },
            async showInformationMessage(message)
            {
                state.infos.push(message);
                return queueValue(state.infoChoice);
            },
            async showErrorMessage(message)
            {
                state.errors.push(message);
                return queueValue(state.errorChoice);
            },
            async showWarningMessage(message)
            {
                state.warnings.push(message);
                return queueValue(state.warningChoice);
            },
            setStatusBarMessage(message)
            {
                state.status.push(message);
                return {dispose() {}};
            },
            createWebviewPanel(viewType, title, showOptions, panelOptions)
            {
                const messageEmitter = new MockEventEmitter();
                const disposeEmitter = new MockEventEmitter();
                const panel = {
                    viewType,
                    title,
                    showOptions,
                    panelOptions,
                    revealOptions: [],
                    webview: {
                        html: "",
                        cspSource: "vscode-webview:",
                        asWebviewUri: (uri) => `vscode-resource:${uri.fsPath}`,
                        onDidReceiveMessage: messageEmitter.event,
                        __fireMessage: (message) => messageEmitter.fire(message)
                    },
                    reveal(...args) { this.revealOptions.push(args); },
                    onDidDispose: disposeEmitter.event,
                    dispose()
                    {
                        disposeEmitter.fire();
                        messageEmitter.dispose();
                        disposeEmitter.dispose();
                    }
                };
                state.panels.push(panel);
                return panel;
            },
            async withProgress(_progressOptions, task)
            {
                const cancellation = new MockEventEmitter();
                return task({}, {
                    isCancellationRequested: false,
                    onCancellationRequested: cancellation.event
                });
            },
            async showTextDocument(document, showOptions)
            {
                const editor = {document, showOptions};
                state.shownDocuments.push(editor);
                vscode.window.activeTextEditor = editor;
                return editor;
            }
        },
        workspace: {
            workspaceFolders: options.noWorkspace
                ? []
                : workspaceRoots.map((root, index) => ({
                    uri: MockUri.file(root),
                    name: path.basename(root),
                    index
                })),
            getConfiguration(_section, resource)
            {
                const folder = resource && vscode.workspace.getWorkspaceFolder(resource);
                const scoped = folder && configurationsByRoot.get(folder.uri.fsPath);
                return {
                    get(key, fallback)
                    {
                        if (scoped?.has(key)) {
                            return scoped.get(key);
                        }
                        return configuration.has(key) ? configuration.get(key) : fallback;
                    },
                    async update(key, value)
                    {
                        if (state.throwConfigUpdate) {
                            throw state.throwConfigUpdate;
                        }
                        (scoped || configuration).set(key, value);
                        configEmitter.fire({
                            affectsConfiguration: (name) =>
                                name.includes(key) || name.endsWith("autoRunOnSave")
                        });
                    }
                };
            },
            onDidSaveTextDocument: saveEmitter.event,
            onDidChangeConfiguration: configEmitter.event,
            getWorkspaceFolder(uri)
            {
                if (!uri) {
                    return vscode.workspace.workspaceFolders[0];
                }
                const candidate = path.resolve(uri.fsPath);
                return vscode.workspace.workspaceFolders
                    .filter((folder) => {
                        const relative = path.relative(folder.uri.fsPath, candidate);
                        return relative === "" || (!relative.startsWith(".." + path.sep) &&
                            relative !== ".." && !path.isAbsolute(relative));
                    })
                    .sort((left, right) =>
                        right.uri.fsPath.length - left.uri.fsPath.length
                    )[0];
            },
            asRelativePath(uri)
            {
                const folder = vscode.workspace.getWorkspaceFolder(uri);
                return path.relative(folder?.uri.fsPath || workspaceRoot, uri.fsPath);
            },
            async findFiles(pattern)
            {
                const basename = String(pattern).replace(/^\*\*\//, "").replace(/\.ct$/, "");
                return workspaceRoots.flatMap((root) => fs.readdirSync(root)
                    .filter((file) => file.endsWith(".ct") && file.startsWith(basename))
                    .map((file) => MockUri.file(path.join(root, file))));
            },
            async openTextDocument(uri)
            {
                const document = {
                    uri,
                    fileName: uri.fsPath,
                    getText: () => fs.readFileSync(uri.fsPath, "utf8")
                };
                state.openedDocuments.push(document);
                return document;
            },
            __fireSave(uri)
            {
                saveEmitter.fire({uri});
            }
        },
        commands: {
            registerCommand(name, callback)
            {
                commandCallbacks.set(name, callback);
                return {dispose: () => commandCallbacks.delete(name)};
            },
            async executeCommand(name, ...args)
            {
                if (!commandCallbacks.has(name)) {
                    state.executedCommand = {name, args};
                    return undefined;
                }
                return commandCallbacks.get(name)(...args);
            }
        },
        debug: {
            activeDebugSession: undefined,
            onDidStartDebugSession: debugStartEmitter.event,
            onDidTerminateDebugSession: debugTerminateEmitter.event,
            registerDebugConfigurationProvider: () => ({dispose() {}}),
            registerDebugAdapterDescriptorFactory: () => ({dispose() {}}),
            async startDebugging(folder, config)
            {
                if (state.throwDebugStart) {
                    throw state.throwDebugStart;
                }
                const session = {
                    id: `mock-session-${++state.sessionCounter}`,
                    type: config.type,
                    configuration: config,
                    workspaceFolder: folder
                };
                state.debugStarts.push(session);
                vscode.debug.activeDebugSession = session;
                debugStartEmitter.fire(session);
                return true;
            },
            async stopDebugging(session)
            {
                if (state.throwDebugStop) {
                    throw state.throwDebugStop;
                }
                const target = session || vscode.debug.activeDebugSession;
                state.debugStops.push(target);
                if (target && !state.suppressDebugTermination) {
                    debugTerminateEmitter.fire(target);
                }
                if (vscode.debug.activeDebugSession === target) {
                    vscode.debug.activeDebugSession = undefined;
                }
            }
        },
        __state: state
    };
    return vscode;
}

function loadExtensionWithMock(extensionPath, vscode)
{
    const resolved = require.resolve(extensionPath);
    delete require.cache[resolved];
    const originalLoad = Module._load;
    Module._load = function(request, parent, isMain) {
        if (request === "vscode") {
            return vscode;
        }
        return originalLoad.call(this, request, parent, isMain);
    };
    try {
        return require(resolved);
    } finally {
        Module._load = originalLoad;
    }
}

module.exports = {
    MockEventEmitter,
    MockUri,
    createContext,
    createVscodeMock,
    loadExtensionWithMock
};
