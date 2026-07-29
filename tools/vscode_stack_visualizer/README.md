# AtomicProof Stack Visualizer VS Code Extension

This is a lightweight VS Code Webview extension for AtomicProof stack traces.
It visualizes `stack_trace.json` inside the editor.

## Run in VS Code

1. Open this repository in VS Code.
2. Open `tools/vscode_stack_visualizer` as the extension development folder, or
   run VS Code's `Developer: Install Extension from Location...` command and
   choose this directory.
3. Use one of these commands from the command palette:
   - `AtomicProof: Open Stack Trace Visualizer`
   - `AtomicProof: Visualize Active Stack Trace`
   - `AtomicProof: Generate Stack Trace and Visualize`
   - `AtomicProof: Open Last Generated Stack Trace`
   - `AtomicProof: Debug Active Stack Trace`
   - `AtomicProof: Debug Live VM`
   - `AtomicProof: Toggle Auto Debug On Save`
   - `AtomicProof: Restart Current Live VM Debug`

You can also right-click a `.ct` contract file and choose
`AtomicProof: Generate Stack Trace and Visualize` or
`AtomicProof: Debug Live VM`, or right-click a trace JSON file and choose
`AtomicProof: Visualize Active Stack Trace` or `AtomicProof: Debug Active Stack Trace`.

## Generate a Trace Manually

```bash
build/bin/utxo_Interpreter run test/debugger_regression/debug_stack_visualizer_alt.ct test_alt_roundtrip 5 --stack-trace-output stack_trace.json
```

Then open `stack_trace.json` in VS Code and run:

```text
AtomicProof: Visualize Active Stack Trace
```

## Generate From VS Code

Run:

```text
AtomicProof: Generate Stack Trace and Visualize
```

The command asks for:

- a `.ct` contract file
- function name
- function arguments
- trace output path

It uses `build/bin/utxo_Interpreter`, writes the JSON trace, and opens the
Webview automatically.

## Auto Debug On Save

Run `AtomicProof: Toggle Auto Debug On Save` to enable or disable save-driven
debugging for `.ct` files. The default mode is `trace`: after a short debounce,
the extension reuses the most recent function name and arguments, regenerates
the configured `traceOutputPath`, and refreshes the visualizer without taking
focus from the editor. Function and argument history is isolated per contract,
and the default output is `<contract>.stack_trace.json` beside that contract so
parallel saves do not share one output file. If no function or argument history
exists yet, VS Code asks for it once.

Set `atomicProofStackVisualizer.autoRunOnSave.mode` to `live` to restart the
matching live VM debug session after saving. The extension records the most
recent live debug launch for each contract and reuses its `functionName`,
`arguments`, `txFile`, and `interpreterPath`. It does not hot-reload the running
VM process; it stops the old session and starts a new one, so VS Code source
breakpoints are preserved and re-applied by the debugger.

## Settings

- `atomicProofStackVisualizer.interpreterPath`: override the interpreter path.
- `atomicProofStackVisualizer.traceOutputPath`: default trace output path.
- `atomicProofStackVisualizer.defaultFunction`: default function name.
- `atomicProofStackVisualizer.defaultArguments`: default function arguments.
- `atomicProofStackVisualizer.autoOpenGeneratedTrace`: open generated traces.
- `atomicProofStackVisualizer.defaultViewMode`: `diff`, `before`, or `after`.
- `atomicProofStackVisualizer.playbackSpeed`: initial playback speed.
- `atomicProofStackVisualizer.openBeside`: open the Webview beside the editor.
- `atomicProofStackVisualizer.autoRunOnSave.enabled`: enable save-driven trace
  refresh or live debug restart.
- `atomicProofStackVisualizer.autoRunOnSave.mode`: `trace` or `live`.
- `atomicProofStackVisualizer.autoRunOnSave.debounceMs`: save debounce delay.
- `atomicProofStackVisualizer.autoRunOnSave.showStatus`: show status bar
  feedback.
- `atomicProofStackVisualizer.autoRunOnSave.restartLiveDebug`: restart live VM
  sessions when auto mode is `live`.

Path settings support `${workspaceFolder}`, `${file}`, `${fileDirname}`,
`${fileBasenameNoExtension}`, and `${home}`.
In a multi-root workspace, settings and `${workspaceFolder}` are resolved from
the workspace folder that owns the selected contract or trace.

## Checks And Packaging

From `tools/vscode_stack_visualizer`, run:

```bash
npm run check
```

This checks the extension script, the bundled visualizer HTML, the trace JSON
schema, and all example traces.

To run a real browser smoke test for the bundled Webview:

```bash
npm run browser-smoke
```

To smoke-test the interpreter JSONL live debug protocol directly:

```bash
npm run live-smoke
```

To run the VS Code Extension Host smoke test:

```bash
npm run extension-smoke
```

On Linux CI, run it under Xvfb:

```bash
xvfb-run -a npm run extension-smoke
```

To build a local VSIX package:

```bash
npm run package
```

The package command uses the repository's local VSIX packer, so it does not
depend on a floating global `vsce` release.

## Automated Test Suites

自动化测试采用分层结构。先安装依赖，再运行与改动对应的套件：

```bash
npm ci
npm run test:fast
npm run test:webview
npm run test:extension
npm run test:live
npm run test:package
npm run test:full
```

`test:fast` 覆盖 Schema/数据不变量、8 个公开命令路径和 Trace/Live DAP 协议；
`test:webview` 使用无头 Chromium 覆盖真实 nonce CSP、离线交互、安全、剪贴板、持久化和
可访问性；`test:extension` 运行真实 VS Code Extension Host；`test:live` 要求
已经构建 `build/bin/utxo_Interpreter`；`test:package` 检查 VSIX 内容并安装到
干净 Extension Host。核心套件缺少 Chromium 或解释器时会明确失败。

10,000 step、1,000 元素栈和连续播放性能检查单独运行：

```bash
npm run test:performance
```

Linux 上先用 `npx playwright-core install chromium` 安装浏览器；没有显示服务时，
Extension Host 测试需要运行在 Xvfb 中：

```bash
xvfb-run -a npm run test:full
```

当前 CI 文件是 `.github/workflows/stack-visualizer-check.yml`。PR 运行 Linux
核心套件，定时任务运行跨平台与性能套件。详细覆盖矩阵见 `TESTING.md`。

## Debug Adapter

The extension contributes a read-only `atomicproof-trace` debugger. Open a
`stack_trace.json` file and run:

```text
AtomicProof: Debug Active Stack Trace
```

Or add a launch configuration:

```json
{
  "type": "atomicproof-trace",
  "request": "launch",
  "name": "Debug AtomicProof Stack Trace",
  "tracePath": "${file}"
}
```

The adapter replays the existing trace offline. The extension assigns `.ct`
files to the `atomicproof-contract` language so traced contract source lines can
host breakpoints. It supports step, reverse step, continue, reverse continue,
source breakpoints by traced source line,
conditional breakpoints, hit-count breakpoints, stack frames, and Variables
scopes for instruction metadata, main stack, alt stack, and stack effects.

Watch/evaluate supports safe trace expressions such as `pc`, `opcode`, `line`,
`functionName`, `main.length`, `alt.length`, `main[0]`, `main[0].hex`,
`alt[0].intString`, `effects.moves`, and `json`.

## Live VM Debugger

The extension also contributes an `atomicproof-live` debugger. It launches the
interpreter in JSONL protocol mode:

```bash
build/bin/utxo_Interpreter debug-server <contract.ct> [function] [args...]
```

From VS Code, right-click a `.ct` file and run:

```text
AtomicProof: Debug Live VM
```

Or add a launch configuration:

```json
{
  "type": "atomicproof-live",
  "request": "launch",
  "name": "Live Debug AtomicProof Contract",
  "contractPath": "${file}",
  "functionName": "test_alt_roundtrip",
  "arguments": ["5"]
}
```

The live debugger compiles the contract, initializes the VM stack from function
arguments, supports source line breakpoints, `continue`, `next`, `stepIn`,
`stepOut`, asynchronous pause, and source-qualified breakpoints. Call Stack
shows all active function frames. Variables includes Locals, Globals,
Instruction, Main Stack, Alt Stack, Call Stack, Warnings, and Errors; Locals and
Watch evaluation follow the selected frame. Watch expressions include source
variables as well as `pc`, `opcode`, `line`, `functionName`, `main[0].hex`,
`alt.length`, and `json`. Unknown expressions return a debugger error instead
of being echoed as a successful value.

`continue` returns before execution finishes and emits the DAP `continued`
event. A subsequent Pause request is handled at the next VM instruction
boundary. Runtime failures stop with reason `exception`, appear in the Debug
Console and Errors scope, and are available through VS Code's exception-info
request. Inspection and breakpoint reconfiguration requests made while the VM
is running fail explicitly; pause first to obtain a consistent snapshot.

## Webview Navigation

Inside the visualizer, use search and filters to jump across long traces.
Previous/next and playback respect the active search or filter. The Webview
also includes source-line navigation, stack-event navigation, function jumping,
bookmarked steps, and an event rail. In VS Code, `Open Source` jumps from the
current trace step back to the contract source line.

The instruction panel includes readable explanations plus `Copy`, `Copy MD`,
and `Copy Trace MD` actions for demo notes. The whole-trace Markdown report
summarizes diagnostics, function ranges, opcode counts, key events, bookmarks,
and the current step without requiring users to read the raw JSON.

Trace diagnostics are shown inside the Webview so incomplete or older trace
files are easier to recognize. Raw step JSON is collapsed by default for large
trace responsiveness; use `Show JSON` or `Copy JSON` when you need the full
current step payload.

Use `Present` or the `P` key to toggle presentation mode for demos. Large stack
columns use virtualized scrolling, so every value remains reachable without
rendering thousands of DOM nodes at once.

Traces generated by the current interpreter include synthetic stack value
lifecycles. Click a stack card to inspect its `elementId`, origin step, stack
moves, and consumption step.

Use `Mark Step` or the `B` key to bookmark important instructions during
analysis or demos. Bookmarks appear in the source gutter and event rail, can be
filtered with `Bookmarks`, and are persisted per trace in Webview local storage.
