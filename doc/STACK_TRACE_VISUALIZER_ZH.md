# VS Code 栈变化可视化

本项目支持把 VM 每一步执行时的 main stack 和 alt stack 导出为 JSON，并通过 VS Code Webview 插件可视化浏览。
同时，VS Code 插件可以通过 `utxo_compiler debug-server` 启动真正的 live VM 调试会话，直接控制同一个 VM 进程进行 continue/step/breakpoint/variables。

## 生成 stack trace

构建后运行：

```bash
cmake --build build -j2
build/bin/utxo_compiler run <contract.ct> <function> <args...> --stack-trace-output stack_trace.json
```

如果你的本地可执行文件名是 `utxo_interpreter`，同样可以使用：

```bash
build/bin/utxo_interpreter run <contract.ct> <function> <args...> --stack-trace-output stack_trace.json
```

参数规则沿用调试器现有入栈规则：

- 整数：`42`、`-1`
- 十六进制字节：`0x1234abcd`
- 字符串：命令行中需要保留引号，例如 `'"hello"'`
- 结构体参数会按字段顺序展开为多个参数

示例：

```bash
build/bin/utxo_compiler run examples/stack_traces/push_pop_demo.ct push_pop 9 --stack-trace-output examples/stack_traces/push_pop_trace.json
build/bin/utxo_compiler run examples/stack_traces/altstack_move_demo.ct move_between_stacks 1 2 3 4 --stack-trace-output examples/stack_traces/altstack_move_trace.json
build/bin/utxo_compiler run examples/stack_traces/combined_stack_demo.ct mixed 1 2 3 4 --stack-trace-output examples/stack_traces/combined_stack_trace.json
```

导出的 JSON 包含：

- `stepIndex`、`pc`、`instruction`、`opcode`、`operand`
- `source.file`、`source.line`、`functionName`
- `mainStack.before/after`、`altStack.before/after`
- `effects`：`push`、`pop`、`move`
- 栈元素的 `elementId`、`producerStep`、`consumerStep`、`origin`
- 栈元素的 `hex`、`byteLength`、`depth`、`int/intString`、`string`

## 使用 VS Code 插件

插件目录位于：

```text
tools/vscode_stack_visualizer/
```

调试插件：

1. 在 VS Code 中打开 `tools/vscode_stack_visualizer/`
2. 按 `F5` 启动 Extension Development Host
3. 在新窗口中打开 AtomicProofInterpreter 工作区
4. 运行命令面板中的命令

可用命令：

- `AtomicProof: Open Stack Trace Visualizer`：选择一个 `stack_trace.json` 并打开 Webview
- `AtomicProof: Visualize Active Stack Trace`：当前编辑器是 trace JSON 时直接可视化
- `AtomicProof: Debug Active Stack Trace`：用 VS Code Debug 面板回放 trace，支持 step/continue、源码断点和变量视图中的 main/alt stack
- `AtomicProof: Debug Contract Trace`：从 `.ct` 合约直接启动 live Debug，插件会运行 `utxo_compiler debug-server <contract.ct> <function> [args...]`，并在同一个 VM 实例上执行 continue/step/breakpoint/variables
- `AtomicProof: Export Trace Narrative`：导出 Markdown 执行叙事，适合论文、教学、评审和演示材料
- `AtomicProof: Generate Stack Trace and Visualize`：选择合约、输入函数名和参数，自动调用 `build/bin/utxo_interpreter` 或 `build/bin/utxo_compiler` 生成 trace 并打开

`launch.json` 中 `.ct` 调试默认使用 live 后端：

```json
{
  "type": "atomicproof-stack-trace",
  "request": "launch",
  "name": "Live Debug AtomicProof Contract",
  "debugBackend": "live",
  "contractFile": "${file}",
  "functionName": "main",
  "functionArgs": [],
  "stopOnEntry": true
}
```

如需保留旧的“生成 trace 后只读回放”流程，设置：

```json
{
  "debugBackend": "trace"
}
```

如果可执行文件不在默认位置，可以在 VS Code 设置中配置：

```json
{
  "atomicProofStackVisualizer.interpreterPath": "/absolute/path/to/utxo_compiler"
}
```

## Webview 交互

Webview 左侧显示源码并高亮当前执行行，中间显示当前 step/PC/opcode/source 和完整 JSON，右侧显示 Main Stack 和 Alt Stack。

- 栈顶元素显示在上方，并标记 `Top`
- `Before` / `After` / `Diff` 三种视图可切换
- `Prev` / `Next` 可单步浏览
- `Play` / `Pause` 可自动播放
- 时间轴可跳转到任意 step
- 速度滑块控制播放间隔
- push 使用绿色动画，pop/drop 使用红色淡出，main stack 与 alt stack 移动使用蓝色高亮
- 点击栈元素可查看稳定 `elementId`、来源、生产 step、消费 step 和生命周期
- step overview、栈深度时间线和大栈快照使用窗口化渲染，避免大 trace 卡顿
- 可通过 Debug Adapter 把 trace 当作只读 replay 调试会话查看，也可以对 `.ct` 合约启动 live VM 调试
- 可导出 Markdown narrative，包含 step 时间线、effects 和元素生命周期表

## 工程验证

插件侧检查：

```bash
cd tools/vscode_stack_visualizer
npm run check
```

插件真实 Extension Host smoke test：

```bash
cd tools/vscode_stack_visualizer
npm ci
xvfb-run -a npm run test:e2e
```

项目提供 `.github/workflows/stack-visualizer-ci.yml`，用于在 CI 中覆盖：

- C++ 构建
- 示例 trace 重新导出
- `elementId` 和跨栈 move 身份一致性校验
- Webview render smoke test
- Debug Adapter replay smoke test
- Live debug-server / Adapter smoke test
- VS Code Extension Host E2E smoke test
- VSIX 打包

## 示例 trace

示例输入合约放在 `examples/stack_traces/`。构建完成后，可用上面的示例命令生成：

- `push_pop_trace.json`
- `altstack_move_trace.json`
- `combined_stack_trace.json`
