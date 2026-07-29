# AtomicProof Stack Visualizer 用户使用指南

> 适用插件版本：`0.1.0`<br>
> 扩展 ID：`atomicproof.atomicproof-stack-visualizer`<br>
> 本指南依据当前仓库中的实现、清单和测试编写。凡无法由代码确认的信息均标为“待确认”。

AtomicProof Stack Visualizer 有两种工作模式，请先区分：

| 模式 | 数据来源 | 界面 | 是否需要解释器 |
| --- | --- | --- | --- |
| 离线 Trace 可视化/调试 | `run ... --stack-trace-output` 生成的 JSON 文件 | 三栏 Webview，或 VS Code 标准调试视图 | 打开已有 Trace 时不需要；生成 Trace 时需要 |
| Live VM 实时调试 | 插件启动 `utxo_Interpreter debug-server` 子进程 | VS Code 标准“运行和调试”视图 | 需要 |

Live VM 通过子进程的 stdin/stdout 交换逐行 JSON（JSONL），不监听 TCP、WebSocket 或其他网络端口。

---

## 1. 插件简介

### 1.1 插件解决什么问题

AtomicProof 合约编译为 Bitcoin-Script-compatible 字节码后，变量、表达式和函数调用最终都表现为栈操作。只阅读 `.ct` 源码或字节码，通常很难直观看出：

- 某条指令消费了哪些值；
- 新值被压到了什么位置；
- 值何时在主栈和 alt stack 之间移动；
- 一个源码行对应了哪些字节码 PC；
- 当前作用域中的变量实际映射到了哪些栈元素；
- 条件分支为什么得到意外结果。

AtomicProof Stack Visualizer 将解释器生成的执行 Trace 显示为源码、指令和双栈视图，并提供 VS Code 调试器集成。

### 1.2 主栈与 alt stack

- **主栈（Main Stack）**是 Bitcoin Script 执行时存放函数参数、操作数和计算结果的主要后进先出栈。
- **alt stack（备用栈）**是临时存放数据的第二个栈。`OP_TOALTSTACK` 将主栈栈顶移入 alt stack，`OP_FROMALTSTACK` 将其移回主栈。

指令执行时可能压入、弹出、复制、重排或跨栈移动数据，因此每执行一步，栈内容和栈顶位置都可能变化。

AtomicProof 中的变量名可以视为栈位置上的符号标签。一次源码赋值不一定产生独立字节码；反过来，一个源码行也可能对应多条指令。调试时应同时观察：

```text
源码行 → 当前函数/作用域 → PC/指令 → 栈变化 → 变量映射
```

### 1.3 主要功能

- 从 `.ct` 合约生成 `apc-stack-trace` JSON。
- 在 Webview 中显示源码、PC、opcode、operand、主栈和 alt stack。
- 对比指令执行前、执行后和变化差异。
- 搜索、过滤、时间轴播放、函数跳转和书签。
- 显示栈值的十六进制、可解析整数、ASCII、字节数和生命周期。
- 离线回放已有 Trace，并支持向前、向后和断点导航。
- 通过 Live VM 调试器进行断点、继续、单步进入、单步跳过、单步跳出和异步暂停。
- 在 Live 模式中查看 Locals、Globals、Instruction、双栈、调用栈、Warnings 和 Errors。
- 保存 `.ct` 后自动重新生成 Trace，或重启已有 Live 会话。

### 1.4 适用场景

- 定位操作数顺序错误；
- 分析主栈与 alt stack 往返；
- 检查函数调用和局部变量；
- 对比条件分支和循环执行路径；
- 调试交易上下文及内建对象；
- 演示或评审合约的逐步执行过程。

> 实现依据：`tools/vscode_stack_visualizer/package.json`、`tools/vscode_stack_visualizer/extension.js`、`tools/vscode_stack_visualizer/stack_visualizer/index.html`、`src/debugger/protocol/live_debug_server.cpp`、`project_doc/DESIGN_PHILOSOPHY.md`。

---

## 2. 环境要求

| 项目 | 要求 |
| --- | --- |
| 编辑器 | Visual Studio Code |
| VS Code 版本 | `1.86.0` 或更高的 1.x 版本 |
| 插件版本 | 当前仓库为 `0.1.0` |
| 操作系统 | CI 配置覆盖 Ubuntu、Windows、macOS；正式支持范围和最低 OS 版本待确认 |
| AtomicProof Interpreter | 必须提供当前 Trace 格式及 `debug-server` JSONL 协议；正式最低版本待确认 |
| 解释器构建 | 生成 Trace 和 Live 调试必须使用 `BUILD_DEBUGGER=ON` |
| 解释器源码构建 | CMake ≥ 3.28、支持 C++20 的编译器、线程库 |
| 插件源码构建 | Node.js 22 是当前 CI 已验证环境；正式最低 Node.js 版本待确认 |
| 插件运行时 | 无需单独安装 Chromium、Playwright 或 npm 依赖 |
| 离线查看已有 Trace | 不需要解释器 |

当前工作区中的解释器执行 `--version` 自报版本为 `1.0.0`，但插件没有执行版本检查，也没有协议版本协商，因此不能据此推导正式最低兼容版本。实际要求是解释器至少具备：

- `run ... --stack-trace-output`；
- `format: "apc-stack-trace"` 的 Trace 输出；
- `debug-server`；
- 当前插件使用的 JSONL 请求和事件。

默认构建方式：

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_DEBUGGER=ON

cmake --build build --parallel
```

默认可执行文件：

```text
Linux/macOS: build/bin/utxo_Interpreter
Windows:     build/bin/utxo_Interpreter.exe
```

> 注意：如果使用 `-DBUILD_DEBUGGER=OFF`，`run` 和 `debug-server` 都不能用于本插件。Live 模式会输出 `debug-server requires BUILD_DEBUGGER=ON` 并退出。

> 实现依据：`tools/vscode_stack_visualizer/package.json`、`.github/workflows/stack-visualizer-check.yml`、`CMakeLists.txt`、`main.cpp`。

---

## 3. 安装插件

### 3.1 从插件市场安装

**状态：待确认。**

当前仓库没有 VS Code Marketplace 页面地址、下载地址、`repository` 或 `homepage` 元数据，也没有发布记录。Manifest 中的 publisher 为 `atomicproof`，但这不能证明插件已经发布到市场。

在维护者提供正式市场页面前，请使用本地 VSIX 或源码安装，不要从非官方地址下载同名扩展。

### 3.2 从本地安装包安装

当前仓库包含：

```text
tools/vscode_stack_visualizer/atomicproof-stack-visualizer-0.1.0.vsix
```

安装步骤：

1. 打开终端。
2. 进入仓库根目录。
3. 执行：

```bash
code --install-extension \
  tools/vscode_stack_visualizer/atomicproof-stack-visualizer-0.1.0.vsix \
  --force
```

4. 重新加载 VS Code 窗口。

也可以在 VS Code 中执行标准命令：

```text
Extensions: Install from VSIX...
```

然后选择该文件。

### 3.3 从源码构建安装

1. 进入扩展目录：

```bash
cd tools/vscode_stack_visualizer
```

2. 安装锁定版本的开发依赖：

```bash
npm ci
```

3. 执行清单、脚本、Schema 和示例检查：

```bash
npm run check
```

4. 生成 VSIX：

```bash
npm run package
```

5. 安装生成的包：

```bash
code --install-extension \
  ./atomicproof-stack-visualizer-0.1.0.vsix \
  --force
```

该打包流程使用仓库内的 VSIX 打包脚本，不依赖全局 `vsce`。

如果只是本地开发，也可以在 VS Code 命令面板执行：

```text
Developer: Install Extension from Location...
```

然后选择：

```text
tools/vscode_stack_visualizer
```

### 3.4 如何确认安装成功

1. 打开命令面板。
2. 输入 `AtomicProof:`。
3. 确认出现以下 8 个命令：

```text
AtomicProof: Open Stack Trace Visualizer
AtomicProof: Visualize Active Stack Trace
AtomicProof: Generate Stack Trace and Visualize
AtomicProof: Open Last Generated Stack Trace
AtomicProof: Debug Active Stack Trace
AtomicProof: Debug Live VM
AtomicProof: Toggle Auto Debug On Save
AtomicProof: Restart Current Live VM Debug
```

4. 打开包含 `.ct` 文件的工作区。
5. 确认状态栏出现 `AtomicProof Trace`，或打开 `.ct` 文件的右键菜单，确认存在：

```text
AtomicProof: Generate Stack Trace and Visualize
AtomicProof: Debug Live VM
```

插件没有安装成功欢迎页，也没有贡献插件级默认快捷键。

<!-- 截图：命令面板中的 AtomicProof 插件命令 -->

> 实现依据：`tools/vscode_stack_visualizer/package.json`、`tools/vscode_stack_visualizer/README.md`、`tools/vscode_stack_visualizer/scripts/package_vsix.js`、`tools/vscode_stack_visualizer/test/package/package_test.js`。

---

## 4. 配置解释器

### 4.1 设置 `utxo_Interpreter` 路径

如果不配置，插件默认使用第一个工作区中的：

```text
Linux/macOS: ${workspaceFolder}/build/bin/utxo_Interpreter
Windows:     ${workspaceFolder}\build\bin\utxo_Interpreter.exe
```

配置步骤：

1. 打开 VS Code Settings。
2. 搜索：

```text
AtomicProof Stack Visualizer: Interpreter Path
```

3. 填入可执行文件路径。

也可以编辑工作区的 `.vscode/settings.json`。

Linux/macOS 示例：

```json
{
  "atomicProofStackVisualizer.interpreterPath": "${workspaceFolder}/build/bin/utxo_Interpreter",
  "atomicProofStackVisualizer.traceOutputPath": "${workspaceFolder}/stack_trace.json"
}
```

Windows 示例：

```json
{
  "atomicProofStackVisualizer.interpreterPath": "${workspaceFolder}\\build\\bin\\utxo_Interpreter.exe",
  "atomicProofStackVisualizer.traceOutputPath": "${workspaceFolder}\\stack_trace.json"
}
```

Windows JSON 中的反斜杠必须写成 `\\`。也可以使用绝对路径：

```json
{
  "atomicProofStackVisualizer.interpreterPath": "C:\\AtomicProof\\build\\bin\\utxo_Interpreter.exe"
}
```

路径设置支持：

```text
${workspaceFolder}
${file}
${fileDirname}
${fileBasename}
${fileBasenameNoExtension}
${home}
```

其中 `${fileBasename}` 已由代码实现，但 Manifest 描述中遗漏了它。

相对路径统一相对于第一个工作区根目录。多根工作区不会自动按当前 `.ct` 文件选择根目录。

### 4.2 验证解释器

Linux/macOS：

```bash
./build/bin/utxo_Interpreter --version
```

Windows PowerShell：

```powershell
.\build\bin\utxo_Interpreter.exe --version
```

验证 Trace 功能：

```bash
build/bin/utxo_Interpreter run \
  test/debugger_regression/debug_stack_visualizer_alt.ct \
  test_alt_roundtrip 5 \
  --stack-trace-output stack_trace.json
```

> 当前 `--help` 没有列出内部使用的 `debug-server`，但命令已经由 `main.cpp` 实现并有回归测试。不能仅凭帮助输出中缺少该命令判断 Live 功能不可用。

### 4.3 stdlib 路径

插件没有独立的 stdlib 设置。stdlib 由 AtomicProof Interpreter 查找，实际顺序是：

1. `APC_STDLIB_PATH` 环境变量；
2. 工作目录下 `user_preferences.json` 的 `paths.stdlib`；
3. 根据解释器位置推导的安装、便携或开发目录；
4. 编译时注入的 `APC_DEFAULT_STDLIB_PATH`；
5. 当前工作目录下的 `stdlib/`。

插件启动解释器时，工作目录通常是第一个工作区根目录。因此 `user_preferences.json` 应放在该目录。

示例：

```json
{
  "paths": {
    "stdlib": "/absolute/path/to/AtomicProofInterpreter/stdlib"
  }
}
```

Windows：

```json
{
  "paths": {
    "stdlib": "C:\\AtomicProof\\stdlib"
  }
}
```

Linux/macOS 可在启动 VS Code 前设置：

```bash
export APC_STDLIB_PATH="/absolute/path/to/AtomicProofInterpreter/stdlib"
code .
```

Windows PowerShell：

```powershell
$env:APC_STDLIB_PATH = "C:\AtomicProof\stdlib"
code .
```

环境变量需要存在于启动 VS Code 的进程环境中。插件创建的解释器子进程继承 VS Code 环境。

相对 import 则相对于包含该 import 的 `.ct` 文件目录解析。

### 4.4 工作区配置示例

`.vscode/settings.json`：

```json
{
  "atomicProofStackVisualizer.interpreterPath": "${workspaceFolder}/build/bin/utxo_Interpreter",
  "atomicProofStackVisualizer.traceOutputPath": "${workspaceFolder}/.atomicproof/stack_trace.json",
  "atomicProofStackVisualizer.defaultFunction": "test_alt_roundtrip",
  "atomicProofStackVisualizer.defaultArguments": "5",
  "atomicProofStackVisualizer.defaultViewMode": "diff",
  "atomicProofStackVisualizer.playbackSpeed": 1,
  "atomicProofStackVisualizer.openBeside": true,
  "atomicProofStackVisualizer.autoRunOnSave.enabled": false
}
```

Live VM 的交易上下文不能通过快捷命令填写，应写入 `.vscode/launch.json`：

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "atomicproof-live",
      "request": "launch",
      "name": "Live Debug AtomicProof Contract",
      "contractPath": "${file}",
      "functionName": "test_alt_roundtrip",
      "arguments": ["5"],
      "txFile": "${workspaceFolder}/context.json",
      "interpreterPath": "${workspaceFolder}/build/bin/utxo_Interpreter"
    }
  ]
}
```

### 4.5 配置错误时的表现

| 配置错误 | 表现 |
| --- | --- |
| 可执行文件不存在 | `Executable not found: ... Build the project first or set atomicProofStackVisualizer.interpreterPath.` |
| `BUILD_DEBUGGER=OFF` | `run requires BUILD_DEBUGGER=ON` 或 `debug-server requires BUILD_DEBUGGER=ON` |
| Trace 目录不可写 | 解释器返回 `trace_write_error`，插件显示生成失败 |
| stdlib 未找到 | `import: cannot locate stdlib root ...` |
| import 模块不存在 | `import: module '...' not found at ...` |
| 多根工作区路径错误 | 插件可能使用第一个工作区而不是当前文件所在工作区 |
| Windows 反斜杠未转义 | `settings.json`/`launch.json` 无法解析，或路径被错误解释 |

> 实现依据：`tools/vscode_stack_visualizer/extension.js` 中的 `getInterpreterPath()`、`resolveConfiguredPath()` 和子进程启动逻辑；stdlib 依据 `src/lexer/import_resolver.cpp`、`src/config/config_manager.cpp`。

---

## 5. 快速开始

下面使用仓库回归测试中的真实合约。它会将值从主栈移入 alt stack，再移回主栈。

### 5.1 准备示例合约

可以直接使用：

```text
test/debugger_regression/debug_stack_visualizer_alt.ct
```

文件内容：

```ct
Contract DebugStackVisualizerAlt:
    def test_alt_roundtrip(input: int):
        temp = Push(10)
        temp2 = Push(150)
        temp3 = temp.Clone() + temp2.Clone()
        SetAlt(temp)
        SetMain(temp)
        result: int = temp + input
```

入口函数和参数：

```text
函数：test_alt_roundtrip
参数：5
```

### 5.2 生成并打开可视化 Trace

1. 打开 AtomicProofInterpreter 仓库工作区。
2. 确保解释器已经以 `BUILD_DEBUGGER=ON` 构建。
3. 打开 `debug_stack_visualizer_alt.ct`。
4. 打开命令面板，执行：

```text
AtomicProof: Generate Stack Trace and Visualize
```

5. 在 `AtomicProof Function` 选择器中选择：

```text
test_alt_roundtrip
```

6. 在参数输入框中填写：

```text
5
```

7. 选择 Trace 输出位置。默认是：

```text
${workspaceFolder}/stack_trace.json
```

8. 等待 `Stack trace generated.`。
9. 插件将自动打开 Webview。

### 5.3 单步查看栈变化

1. 保持视图模式为 `Diff`。
2. 点击 `>>` 查看下一步，或按右方向键。
3. 找到 `OP_TOALTSTACK`，观察值从 Main Stack 移入 Alt Stack。
4. 找到 `OP_FROMALTSTACK`，观察同一个值移回 Main Stack。
5. 点击栈卡，可以查看带 `elementId` 的值生命周期。
6. 点击 `End` 或拖动 `Timeline` 到最后一步。
7. 切换到 `After` 查看最终双栈。

该示例经当前解释器验证后会产生 11 个步骤。最后一步执行 `OP_ADD`，主栈栈顶显示：

```text
0x0f
int=15
```

alt stack 最终为空。主栈中还保留示例产生的其他值，因此这里的“最终结果”应理解为最后一步后的完整栈状态，而不是独立的高级语言返回值面板。

<!-- 截图：主栈与 alt stack 可视化界面 -->

### 5.4 启动 Live VM 调试

1. 回到 `.ct` 编辑器。
2. 如需断点，先在希望暂停的可执行源码行左侧单击。
3. 执行：

```text
AtomicProof: Debug Live VM
```

4. 选择 `test_alt_roundtrip`。
5. 输入参数 `5`。
6. 调试器会在入口状态暂停。
7. 使用 VS Code 调试工具栏的：

```text
Continue
Pause
Step Over
Step Into
Step Out
Stop
```

8. 在 Variables 中展开：

```text
Locals
Globals
Instruction
Main Stack
Alt Stack
Call Stack
Warnings
Errors
```

9. 程序结束后，如需重新调试，执行：

```text
AtomicProof: Restart Current Live VM Debug
```

<!-- 截图：VS Code 实时调试的 Variables 和 Call Stack -->

> 实现依据：`test/debugger_regression/debug_stack_visualizer_alt.ct`、`test/debugger_regression/run_debugger_regression.sh`、`examples/stack_traces/alt_roundtrip.json`。上述 Trace 命令及 JSONL Live smoke 已在当前工作区实际验证通过。

---

## 6. 界面说明

### 6.1 Webview 三栏界面

| 区域 | 显示什么 | 如何操作 | 如何理解 |
| --- | --- | --- | --- |
| Source | Trace 内嵌的 `.ct` 源码、行号、当前行和书签 | 点击源码行或 `Open Source` 回到编辑器 | 一个源码行可能对应多个 PC；当前行表示当前指令映射到的源位置 |
| 当前执行位置 | 当前步数、总步数、PC、opcode、operand、source | 使用 `<<`、`>>`、Timeline 或导航按钮移动 | Trace step 是执行序号；PC 是字节码位置，两者不是同一概念 |
| Main Stack | 指令前后主栈内容和变化 | 滚动、点击栈卡、切换 Diff/Before/After | 栈顶位于上方；`depth=0` 表示栈顶 |
| Alt Stack | 备用栈内容和跨栈移动 | 使用 `Alt stack` 过滤器或事件导航 | 主要关注 `OP_TOALTSTACK`、`OP_FROMALTSTACK` 及跨栈 move |
| Instruction | 当前 opcode、operand、函数、源位置和栈效果 | 切换 Explanation、Details、JSON | 一条指令可能弹出多个值并压入新值 |
| Scope | 当前函数选择器 | 从下拉列表跳到函数起始处 | 此处的 `Scope` 不是 Locals/Globals 浏览器 |
| Explanation | 当前步骤的文字说明 | `Copy`、`Copy MD`、`Copy Trace MD` | 用于快速理解和输出调试记录 |
| Details | 步骤、事件、书签、栈大小、函数范围和 Trace Diagnostics | 查看异常或不完整元数据 | Diagnostics 反映 Trace 完整度，不等于 VM 运行结果 |
| JSON | 当前步骤原始 JSON | `Show JSON`、`Copy JSON` | 用于精确检查 before/after、effects 和 source 字段 |
| 事件轨道 | push、pop、move、alt、error 和书签事件 | 点击圆点跳转 | 适合快速定位长 Trace 中的关键步骤 |

Webview 没有独立的完整字节码列表。Trace 顶层即使包含 `bytecode`，也不会作为完整指令区渲染。

### 6.2 变量、作用域和符号信息

Webview 本身不显示 Locals、Globals 或符号表。变量与作用域信息位于 VS Code 标准调试视图：

- 离线 Trace 调试器：

```text
Instruction
Main Stack After
Alt Stack After
Effects
```

- Live VM 调试器：

```text
Locals
Globals
Instruction
Main Stack
Alt Stack
Call Stack
Warnings
Errors
```

Live 模式会按当前选中的调用帧读取变量。被优化、离开作用域或已被栈操作消费的变量可能显示为不可用。

当前源码行不一定对应单一指令。Live 的 `Instruction` scope 会显示同一源码行映射的 PC 摘要，当前 PC 用 `*` 标识。

### 6.3 栈卡内容

一张离线 Trace 栈卡可能包含：

```text
hex        原始十六进制字节
top        当前栈顶
depth=N    距离栈顶的深度
id=...     合成的生命周期标识
int=...    可按 ScriptNum 解码时的整数
str="..."  可打印 ASCII
N bytes    字节长度
```

`elementId` 是可视化器根据值和位置合成的生命周期标识，不是 VM 原生对象 ID，也不是密码学唯一标识。出现重复值时，不应仅凭 ID 推断业务身份。

### 6.4 常见颜色和图标

| 颜色/标记 | 含义 |
| --- | --- |
| 绿色 `pushed` | 本步骤新压入的值 |
| 红色 `popped` | 本步骤弹出的值或错误诊断 |
| 黄色移动标记 | 主栈与 alt stack 之间的移动 |
| 青蓝色 alt 标记 | alt stack 的移入、移出或相关事件 |
| 蓝色背景和 `>` | 当前源码行 |
| 蓝色轮廓 | 当前选中的栈值、搜索命中或当前事件 |
| 粉色 `B` | 书签源码行 |
| 粉色圆点/外圈 | 书签步骤 |
| 红色事件圆点 | 错误步骤 |
| 绿色 Diagnostics | Trace 元数据完整 |
| 黄色 Diagnostics | 警告 |
| 红色 Diagnostics | 错误 |

### 6.5 日志和错误信息

插件创建的 Output Channel 名称为：

```text
AtomicProof Stack Visualizer
```

其中包含：

- 实际执行的解释器命令；
- Trace 生成 stdout/stderr；
- 自动保存刷新日志；
- JSONL 非法行；
- 解释器异常退出信息。

Live 运行时错误还会出现在：

- Debug Console；
- `Errors` scope；
- VS Code exception 信息；
- 当前源码停止位置。

> 实现依据：`tools/vscode_stack_visualizer/stack_visualizer/index.html`、`tools/vscode_stack_visualizer/extension.js`、`src/debugger/protocol/live_debug_server.cpp`。

---

## 7. 常用操作

### 7.1 启动、暂停和停止

#### Webview 离线播放

1. 执行 `AtomicProof: Generate Stack Trace and Visualize` 或打开已有 Trace。
2. 点击 `Play` 自动前进。
3. 点击 `Pause` 停止自动回放。

这里的 `Pause` 只暂停离线动画，不会暂停正在运行的 VM。

#### Live VM

1. 执行 `AtomicProof: Debug Live VM`。
2. 使用 VS Code 的 `Continue` 开始执行。
3. 使用 `Pause` 请求在下一个 VM 指令边界暂停。
4. 使用 `Stop` 终止解释器子进程。

VM 运行期间查询变量、求值或重新配置断点可能返回：

```text
program is running; pause it before inspecting or reconfiguring
```

### 7.2 单步进入、单步跳过和继续

| 模式 | Step Into | Step Over | Step Out | Continue |
| --- | --- | --- | --- | --- |
| Live VM | 进入函数调用 | 跳过函数调用 | 执行到当前函数返回 | 运行到断点、异常或结束 |
| 离线 Trace | 前进一个 Trace step | 前进一个 Trace step | 前进一个 Trace step | 前进到下一个断点或末尾 |

离线 Trace 还支持 Step Back 和 Reverse Continue；Live VM 不支持反向执行。

### 7.3 设置或删除断点

1. 打开对应 `.ct` 文件。
2. 单击编辑器行号左侧设置断点。
3. 再次单击删除断点。
4. 启动离线 Trace 调试或 Live VM 调试。

注意：

- 只有映射到实际 AtomicProof 指令的源码行才能验证成功。
- 无法映射的行会显示未验证断点。
- 离线 Trace 支持普通、条件和 hit-count 断点。
- Live VM 面向最终用户只支持普通源码行断点，不支持条件断点、hit count 或可靠的 Logpoint。
- Webview 中不能直接设置源码断点。

### 7.4 重启调试

Live VM：

```text
AtomicProof: Restart Current Live VM Debug
```

该命令会停止旧会话，再使用记录的 `functionName`、`arguments`、`txFile` 和 `interpreterPath` 创建新进程，不是热重载。

离线 Trace 没有独立重启命令。重新执行：

```text
AtomicProof: Debug Active Stack Trace
```

或重新生成 Trace。

### 7.5 查看栈元素详情

在 Webview 中：

1. 点击带 `elementId` 的栈卡。
2. 查看产生、跨栈移动和消费事件。
3. 点击生命周期事件跳转到对应步骤。
4. 点击 `Clear` 清除选择。

在 Live 模式中：

1. 暂停 VM。
2. 展开 `Main Stack` 或 `Alt Stack`。
3. `[0] top` 表示栈顶。
4. 可在 Watch 中输入：

```text
main.length
alt.length
main[0]
main[0].hex
main[0].intString
main[0].depth
alt[0].hex
pc
opcode
line
functionName
json
```

### 7.6 复制数据

Webview 实际提供：

| 按钮 | 复制内容 |
| --- | --- |
| `Copy` | 当前步骤的纯文本说明 |
| `Copy MD` | 当前步骤 Markdown |
| `Copy Trace MD` | 整条 Trace 的 Markdown 报告 |
| `Copy JSON` | 当前步骤原始 JSON |

当前没有直接复制单张栈卡的按钮。如需完整栈元素字段，请使用 `Copy JSON`。

### 7.7 切换数据显示格式

实际支持的栈视图模式：

- `Diff`：突出新增、弹出和跨栈移动；
- `Before`：显示指令执行前的双栈；
- `After`：显示指令执行后的双栈。

实际支持的主题：

- `Light`
- `Dark`
- `VS Code`

当前没有单独的十六进制/十进制切换。栈卡始终显示 hex，并在可解码时附带 `int` 或 ASCII。

### 7.8 查看错误位置

离线 Trace：

1. 将 Filter 设为 `Errors`。
2. 使用 `Prev`/`Next` 跳转。
3. 查看 `Details` 和 `JSON`。
4. 点击 `Open Source`。

Live VM：

1. 查看 Debug Console。
2. 展开 `Errors`。
3. 查看当前 exception 停止位置。
4. 修复后执行 `AtomicProof: Restart Current Live VM Debug`。

### 7.9 Webview 键盘操作

这些按键仅在 Webview 获得焦点时生效，不是插件级 VS Code 快捷键。

| 按键 | 操作 |
| --- | --- |
| `/` | 聚焦搜索框 |
| `Esc` | 清空搜索并离开搜索框 |
| `←` / `→` | 上一步/下一步 |
| `Shift+←` / `Shift+→` | 上/下一个源码行 |
| `↑` / `↓` | 上/下一个栈事件 |
| `B` | 标记/取消当前步骤 |
| `P` | 切换 Present 模式 |
| `[` / `]` | 上/下一个书签 |
| `Space` | 播放/暂停 |
| `Home` / `End` | 第一/最后一步 |
| `PageUp` / `PageDown` | 前后移动 10 步 |

输入框、下拉框或按钮获得焦点时，全局导航按键不会触发。

> 实现依据：`tools/vscode_stack_visualizer/package.json`、`tools/vscode_stack_visualizer/extension.js`、`tools/vscode_stack_visualizer/stack_visualizer/index.html`。

---

## 8. 配置项参考

### 8.1 扩展设置

所有扩展设置均为可选。

| 配置键 | 类型 | 默认值 | 必填 | 作用 | 示例 |
| --- | --- | --- | --- | --- | --- |
| `atomicProofStackVisualizer.interpreterPath` | string | `""` | 否 | 解释器路径；空值使用工作区默认路径 | `${workspaceFolder}/build/bin/utxo_Interpreter` |
| `atomicProofStackVisualizer.traceOutputPath` | string | `${workspaceFolder}/stack_trace.json` | 否 | 默认 Trace 输出文件 | `${workspaceFolder}/.atomicproof/trace.json` |
| `atomicProofStackVisualizer.defaultFunction` | string | `""` | 否 | 函数选择默认值 | `verify` |
| `atomicProofStackVisualizer.defaultArguments` | string | `""` | 否 | 参数输入默认值 | `5 10` |
| `atomicProofStackVisualizer.autoOpenGeneratedTrace` | boolean | `true` | 否 | 生成后自动打开 Webview | `false` |
| `atomicProofStackVisualizer.defaultViewMode` | string | `diff` | 否 | 初始视图：`diff`、`before`、`after` | `after` |
| `atomicProofStackVisualizer.playbackSpeed` | number | `1` | 否 | 初始播放速度，范围 0.25–4 | `2` |
| `atomicProofStackVisualizer.openBeside` | boolean | `true` | 否 | 在当前编辑器旁打开 Webview | `false` |
| `atomicProofStackVisualizer.autoRunOnSave.enabled` | boolean | `false` | 否 | 保存 `.ct` 后自动运行 | `true` |
| `atomicProofStackVisualizer.autoRunOnSave.mode` | string | `trace` | 否 | 保存后执行 `trace` 或 `live` | `live` |
| `atomicProofStackVisualizer.autoRunOnSave.debounceMs` | number | `600` | 否 | 保存后的防抖延迟，最小 0 ms | `1000` |
| `atomicProofStackVisualizer.autoRunOnSave.showStatus` | boolean | `true` | 否 | 显示自动运行状态栏 | `false` |
| `atomicProofStackVisualizer.autoRunOnSave.restartLiveDebug` | boolean | `true` | 否 | Live 自动模式下重启已有会话 | `false` |

开启自动保存调试的命令：

```text
AtomicProof: Toggle Auto Debug On Save
```

行为说明：

- `trace` 模式复用最近使用的函数和参数，重新生成 `traceOutputPath`。
- `live` 模式只重启已经启动或记录过的对应合约会话。
- Live 自动模式不是热重载。
- 如果 `restartLiveDebug` 为 `false`，Live 保存事件会被跳过。

### 8.2 Live VM `launch.json`

| 配置键 | 类型 | 默认值 | 必填 | 作用 | 示例 |
| --- | --- | --- | --- | --- | --- |
| `type` | string | 无 | 是 | 调试器类型 | `atomicproof-live` |
| `request` | string | `launch` | 是 | 启动请求 | `launch` |
| `name` | string | `Live Debug AtomicProof Contract` | 否 | 会话名称 | `Debug verify` |
| `contractPath` | string | `${file}` | 是 | `.ct` 合约路径 | `${workspaceFolder}/contract.ct` |
| `functionName` | string | `""` | 否 | 入口函数；空值由服务端选择第一个 public 函数 | `verify` |
| `arguments` | string 或 string[] | `[]` | 否 | 函数参数 | `["5", "0x01"]` |
| `txFile` | string | `""` | 否 | 交易上下文文件 | `${workspaceFolder}/context.json` |
| `interpreterPath` | string | 扩展设置 | 否 | 覆盖本次会话的解释器路径 | `${workspaceFolder}/build/bin/utxo_Interpreter` |

建议在 `launch.json` 中使用参数数组，避免快捷输入框的简单引号解析限制。

需要保留双引号的文本字节参数可写成：

```json
{
  "arguments": ["\"hello world\""]
}
```

### 8.3 离线 Trace `launch.json`

```json
{
  "type": "atomicproof-trace",
  "request": "launch",
  "name": "Debug AtomicProof Stack Trace",
  "tracePath": "${file}"
}
```

| 配置键 | 类型 | 默认值 | 必填 | 作用 |
| --- | --- | --- | --- | --- |
| `type` | string | 无 | 是 | `atomicproof-trace` |
| `request` | string | `launch` | 是 | 启动请求 |
| `name` | string | `Debug AtomicProof Stack Trace` | 否 | 会话名称 |
| `tracePath` | string | `${file}` | 是 | `apc-stack-trace` JSON 路径 |

> 实现依据：`tools/vscode_stack_visualizer/package.json` 的 `contributes.configuration` 和 `contributes.debuggers`，以及 `tools/vscode_stack_visualizer/extension.js`。

---

## 9. 典型使用场景

### 9.1 定位栈顺序错误

1. 生成 Trace。
2. 切换到 `Diff`。
3. 搜索相关 opcode。
4. 检查红色 `popped` 卡片的顺序。
5. 对照绿色 `pushed` 结果。
6. 使用 `Before` 和 `After` 验证栈顶变化。

`depth=0` 始终表示当前栈顶。对于需要两个操作数的指令，操作数顺序错误往往会直接表现为弹出顺序不符合预期。

### 9.2 排查主栈与 alt stack 不一致

1. 将 Filter 设为 `Alt stack`。
2. 使用 `Prev Event`/`Next Event` 导航。
3. 检查 `OP_TOALTSTACK` 和 `OP_FROMALTSTACK` 是否成对。
4. 点击带 `elementId` 的值，查看其移动历史。
5. 在最后一步切换 `After`，确认 alt stack 是否为空或符合预期。

可直接使用：

```text
examples/stack_traces/alt_roundtrip.json
```

### 9.3 分析函数调用和作用域

1. 启动 Live VM。
2. 在私有函数或调用语句附近设置普通行断点。
3. 使用 `Step Into` 进入函数。
4. 在 Call Stack 中选择不同帧。
5. 比较各帧的 Locals 和 Watch 表达式。
6. 使用 `Step Out` 返回调用者。

Webview 的 Function 下拉框只能跳转到 Trace 中的函数区间，不能替代 Live Call Stack。

### 9.4 检查条件分支

1. 分别使用不同参数生成 Trace。
2. 对比实际出现的源码行和 opcode。
3. 使用 `Stack changes` 过滤器减少无关步骤。
4. 检查条件判断指令前的主栈顶。
5. 将关键判断步骤标记为书签。

仓库中已有：

```text
examples/stack_traces/branch_loop_true.json
examples/stack_traces/branch_loop_false.json
```

Trace 只记录实际执行的路径，不会显示未执行分支的栈状态。

### 9.5 调试交易上下文或内建对象

1. 在 `.vscode/launch.json` 中配置 `txFile`。
2. 启动 `atomicproof-live`。
3. 在读取内建对象的源码行前设置断点。
4. 暂停后查看 Globals、Locals 和 Watch。
5. 查看 Warnings，确认缺失字段是否采用了默认值。
6. 如需离线 Trace，使用解释器手工生成：

```bash
build/bin/utxo_Interpreter run \
  contract.ct verify arg1 arg2 \
  --txfile context.json \
  --stack-trace-output stack_trace.json
```

快捷命令 `AtomicProof: Generate Stack Trace and Visualize` 当前不会询问 `txFile`。

> 实现依据：`examples/stack_traces/README.md`、`tools/vscode_stack_visualizer/README.md`、Live DAP 和 Webview 测试。

---

## 10. 常见问题与故障排查

### 10.1 找不到解释器

- **现象**<br>
  显示：

  ```text
  Executable not found: ...
  Build the project first or set atomicProofStackVisualizer.interpreterPath.
  ```

- **可能原因**<br>
  解释器尚未构建；路径指向错误工作区；Windows 缺少 `.exe`；多根工作区使用了第一个根目录。

- **检查方法**

  ```bash
  ls -l build/bin/utxo_Interpreter
  build/bin/utxo_Interpreter --version
  ```

  Windows：

  ```powershell
  Get-Item .\build\bin\utxo_Interpreter.exe
  .\build\bin\utxo_Interpreter.exe --version
  ```

- **解决办法**<br>
  使用 `BUILD_DEBUGGER=ON` 重新构建，并设置正确的 `atomicProofStackVisualizer.interpreterPath`。

### 10.2 插件无法启动

- **现象**<br>
  命令面板中没有 `AtomicProof:` 命令，右键菜单缺失，或 Webview 报模板不存在。

- **可能原因**<br>
  VS Code 版本低于 1.86；VSIX 未安装成功；扩展包不完整；扩展被禁用。

- **检查方法**

  ```bash
  code --list-extensions --show-versions
  ```

  应出现类似：

  ```text
  atomicproof.atomicproof-stack-visualizer@0.1.0
  ```

- **解决办法**<br>
  升级 VS Code，重新安装 VSIX，并重新加载窗口。源码安装时先运行：

  ```bash
  cd tools/vscode_stack_visualizer
  npm ci
  npm run check
  npm run package
  ```

### 10.3 `.ct` 文件无法解析

- **现象**<br>
  Trace 生成失败，通知显示解释器退出码非零；Live 会话在启动阶段终止。

- **可能原因**<br>
  `.ct` 语法错误；import 失败；合约文件为空；当前解释器不支持所用语法；配置了错误的源文件。

- **检查方法**<br>
  在终端运行与插件等价的命令：

  ```bash
  build/bin/utxo_Interpreter run \
    path/to/contract.ct functionName args \
    --stack-trace-output /tmp/trace.json
  ```

  同时查看 `AtomicProof Stack Visualizer` Output Channel。

- **解决办法**<br>
  按解释器给出的文件、行号和错误信息修复语法或 import。注意：Live 模式下缺失或空 `.ct` 文件可能只表现为 debug server 提前退出，这是当前诊断缺口。

### 10.4 入口函数不存在

- **现象**<br>
  Live server 发送 error 后退出，或 Trace 生成失败；错误中包含函数不存在或没有 debug range。

- **可能原因**<br>
  函数名拼写错误；选择器的正则扫描结果与实际语法结构不一致；函数没有可调试范围。

- **检查方法**<br>
  核对 `.ct` 中的：

  ```ct
  def functionName(...):
  ```

  也可以在函数选择器中使用：

  ```text
  Enter custom function...
  ```

- **解决办法**<br>
  使用准确函数名。`launch.json` 中 `functionName` 为空且参数也为空时，服务端会尝试第一个 public 函数；快捷命令则会要求用户选择函数。

### 10.5 参数类型或数量错误

- **现象**<br>
  出现：

  ```text
  too many arguments
  invalid argument '...' for type '...'
  arguments were provided, but selected function has no parameters
  ```

- **可能原因**<br>
  参数过多；整数、hex 或 address 格式错误；无参数函数却提供了参数；快捷输入中的引号被去掉。

- **检查方法**<br>
  核对函数签名及参数顺序。常用格式：

  ```text
  整数：5
  十六进制：0x0102
  地址：合法 P2PKH 地址
  ```

- **解决办法**<br>
  推荐在 `launch.json` 使用字符串数组。需要保留双引号的文本值使用：

  ```json
  {
    "arguments": ["\"hello world\""]
  }
  ```

  少传参数在 Live 模式下不会直接失败，而会使用 `0x00` 并在 Warnings 中提示；这可能掩盖配置错误，应主动检查 Warnings。

### 10.6 debug server 异常退出

- **现象**<br>
  Debug Console 显示：

  ```text
  AtomicProof live debug server exited with code ...
  ```

  或：

  ```text
  AtomicProof live debug server exited with signal ...
  ```

- **可能原因**<br>
  编译失败、函数或参数错误、`txFile` 不存在、解释器不兼容、`BUILD_DEBUGGER=OFF`，或子进程崩溃。

- **检查方法**

  ```bash
  cd tools/vscode_stack_visualizer
  npm run live-smoke
  ```

  查看 Output Channel 中实际执行的 `debug-server` 命令及 stderr。

- **解决办法**<br>
  使用同一仓库版本重新构建解释器；确认 `BUILD_DEBUGGER=ON`；修复 `txFile`、入口和参数。运行时异常后应重启，而不是继续执行。

### 10.7 JSONL 数据解析失败

- **现象**<br>
  Output Channel 出现：

  ```text
  Invalid live debug protocol line: ...
  ```

  会话可能无法进入 ready 状态或某个请求一直等待。

- **可能原因**<br>
  使用了不兼容的解释器；包装脚本向 stdout 写日志；debug server 将普通文本混入协议输出。

- **检查方法**<br>
  确认 `interpreterPath` 指向真实 `utxo_Interpreter`，而不是会输出额外文本的 shell 包装器。运行：

  ```bash
  cd tools/vscode_stack_visualizer
  npm run live-smoke
  ```

- **解决办法**<br>
  保证 stdout 只包含每行一个 JSON 对象；普通日志应写 stderr。插件当前不会自动重连，也没有协议请求超时。

### 10.8 栈视图不更新

- **现象**<br>
  外部覆盖了 `stack_trace.json`，但已打开 Webview 没有变化；或 Live Variables 在运行时保持旧值。

- **可能原因**<br>
  Webview 是离线快照，没有通用文件监听；自动保存未开启；搜索或过滤器跳过了步骤；Live VM 尚未暂停。

- **检查方法**<br>
  检查：

  ```text
  atomicProofStackVisualizer.autoRunOnSave.enabled
  atomicProofStackVisualizer.autoRunOnSave.mode
  ```

  将 Filter 切回 `All steps`，清空搜索，并暂停 Live VM。

- **解决办法**<br>
  重新执行 `AtomicProof: Visualize Active Stack Trace`，或重新生成 Trace。需要保存刷新时启用 Auto Trace；Live 模式则重启已有会话。

### 10.9 stdlib 或 import 无法找到

- **现象**<br>
  出现：

  ```text
  import: cannot locate stdlib root
  ```

  或：

  ```text
  import: module '...' not found at ...
  ```

- **可能原因**<br>
  `APC_STDLIB_PATH` 未传入 VS Code；`user_preferences.json` 不在工作区根；路径不存在；模块名与 stdlib 目录不匹配。

- **检查方法**

  Linux/macOS：

  ```bash
  test -d "$APC_STDLIB_PATH" && echo ok
  ```

  检查工作区根目录中的：

  ```json
  {
    "paths": {
      "stdlib": "/absolute/path/to/stdlib"
    }
  }
  ```

- **解决办法**<br>
  在启动 VS Code 前设置 `APC_STDLIB_PATH`，或修正 `user_preferences.json`。相对 import 应相对于当前 `.ct` 文件填写。

### 10.10 Windows 路径问题

- **现象**<br>
  提示找不到 `.exe`、配置 JSON 无法解析，或路径中的空格导致用户误判。

- **可能原因**<br>
  未写 `.exe`；JSON 反斜杠未转义；路径指向错误工作区；复制了 Linux 路径。

- **检查方法**

  ```powershell
  Test-Path "C:\AtomicProof\build\bin\utxo_Interpreter.exe"
  ```

  检查配置是否使用双反斜杠：

  ```json
  {
    "atomicProofStackVisualizer.interpreterPath": "C:\\AtomicProof\\build\\bin\\utxo_Interpreter.exe"
  }
  ```

- **解决办法**<br>
  使用正确 `.exe` 路径或 `${workspaceFolder}` 占位符。插件使用参数数组启动解释器，不经过 shell，因此可执行文件路径中的空格不需要额外 shell 转义。

---

## 11. 限制与注意事项

- 插件仅明确声明支持 VS Code；其他兼容编辑器待确认。
- 插件是否已发布到 Marketplace 待确认。
- 没有正式的最低解释器版本或协议协商。
- Webview 是离线 Trace 播放器，不是 Live VM 界面。
- Webview 中没有 Locals、Globals、Call Stack 或符号表。
- Webview 中没有完整字节码列表。
- Webview 不能设置源码断点。
- 没有单个栈值直接复制按钮。
- 没有独立的 hex/decimal 显示切换。
- Live VM 不支持 Step Back 或 Reverse Continue。
- Live VM 不支持面向最终用户的条件断点、hit count 或可靠 Logpoint。
- Live 保存刷新是停止旧进程再启动新进程，不是热重载。
- 外部修改 Trace 文件不会自动刷新已打开的 Webview。
- 插件没有自动重连 debug server。
- Live 快捷命令不提供 `txFile` 输入，必须使用 `launch.json`。
- 插件没有 stdlib 配置键。
- 插件没有提供 `--asa` / `--allow-subscope-altstack` 设置。需要该语义时只能手工生成离线 Trace；Live 插件当前不会附加该参数。
- `.ct` 函数选择使用简单正则扫描，不是完整语法分析。
- 参数输入只实现一层简单引号拆分，不是完整 shell 语法。
- 缺失函数参数会用 `0x00` 补齐并产生 Warning，而不是直接失败。
- 多根工作区统一使用第一个工作区根目录。
- `elementId` 是合成生命周期标识，不应视为 VM 原生唯一 ID。
- Schema 只强制部分字段；Schema 验证通过不代表源码、effects 和 lifecycle 信息完整。
- 当前 `utxo_Interpreter --help` 未列出已经实现的 `debug-server`。
- 仓库没有已提交的正式产品截图资源，本文使用截图占位符。

---

## 12. 反馈问题时需要提供的信息

提交问题时，请尽量附上：

1. 插件版本：

   ```text
   atomicproof.atomicproof-stack-visualizer@0.1.0
   ```

2. VS Code 版本：

   ```bash
   code --version
   ```

3. 操作系统和架构：

   ```text
   例如：Windows 11 x64、Ubuntu x86_64、macOS arm64
   ```

4. 解释器版本：

   ```bash
   build/bin/utxo_Interpreter --version
   ```

5. 解释器是否使用：

   ```text
   BUILD_DEBUGGER=ON
   ```

6. 脱敏后的工作区配置：

   ```text
   .vscode/settings.json
   .vscode/launch.json
   user_preferences.json
   ```

7. 实际使用的命令、入口函数和参数。

8. `AtomicProof Stack Visualizer` Output Channel 内容。

9. Live 问题对应的 Debug Console 内容。

10. 可最小复现的 `.ct` 文件。

11. 若是离线可视化问题，附上对应 Trace JSON。

12. 若涉及 import，提供：

    ```text
    APC_STDLIB_PATH
    paths.stdlib
    import 语句
    ```

13. 清晰的复现步骤、预期结果和实际结果。

Trace、交易上下文和 Debug Console 可能包含合约参数、交易数据或其他敏感信息，提交前应先脱敏。

---

## 13. 待维护者确认的信息

1. 插件是否已经正式发布到 VS Code Marketplace；如已发布，请补充准确页面和下载地址。
2. 正式支持的操作系统、架构及最低系统版本。
3. AtomicProof Stack Visualizer `0.1.0` 对应的最低 AtomicProof Interpreter 版本、tag 或 commit。
4. 是否正式支持 VS Code 之外的兼容编辑器。
5. Node.js 22 是正式源码构建要求，还是仅为当前 CI 已验证版本。
6. 是否应公开并记录实现中已支持的 `${fileBasename}` 路径变量。
7. 是否计划为 Live JSONL 协议增加显式协议版本和能力协商。
8. 是否计划将 `debug-server` 加入 `utxo_Interpreter --help`。
9. 缺失或空 `.ct` 文件在 Live 协议启动前可能静默退出，是否属于已知缺陷。
10. 是否计划在插件配置中提供 `--asa` / `--allow-subscope-altstack`。
11. Live 核心协议已有条件断点逻辑，但 DAP Adapter 当前禁用；是否计划向最终用户开放。
12. 离线 Trace Adapter 的 Logpoint 是否会正式声明并支持。
13. 是否计划提供 Webview 中的完整字节码列表、Locals/Globals、单栈值复制或数值格式切换。
14. 是否会增加 Trace 文件变更监听、Live 热重载或 debug server 自动重连。
15. 是否有正式截图、图标、市场介绍素材和发布下载渠道。
16. Windows、macOS 解释器的签名、权限及正式分发方式。
17. stdin EOF 时 debug server 是否应补发 `terminated` 事件。
18. 是否需要明确规定 Live `disconnect`/`terminate` 最终快照的状态值。
