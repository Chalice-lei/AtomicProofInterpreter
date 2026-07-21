# AGENTS.md

本文件给 Codex 等代码代理使用，适用于整个仓库。

## 交流规则

- 所有面向用户的回答一律使用中文。
- 如果客户端支持会话标题或 ai-title，标题一律使用中文。
- 先阅读现有代码和文档，再改动；不要凭旧的 `CLAUDE.md` 内容直接假设当前行为。
- 不要回滚用户已有改动。遇到无关的脏工作区改动时忽略；遇到会影响当前任务的改动时，基于现状继续处理。

## 项目概览

AtomicProof Interpreter 是一个 C++20 项目，CMake 项目名为 `utxo_interpreter`。它围绕 `.ct` 合约语言提供编译、运行、AST 解释、调试、REPL 和 JSONL debug server 等入口。

主要构建产物：

- 静态库：`apc_core`
- 可执行文件：`build/bin/utxo_Interpreter`

核心能力：

- 将 `.ct` 源码编译为 Bitcoin-Script-compatible bytecode。
- 通过 bytecode runner 或 AST interpreter 运行合约函数。
- 提供交互式 debugger、compiler REPL、shell、stack trace JSON 输出等开发工具。

## 构建

要求 CMake >= 3.28 和 C++20 编译器。`nlohmann/json` 默认通过 CMake 自动获取，也可使用系统安装或本地路径。

常用命令：

```bash
mkdir -p build
cd build
cmake ..
cmake --build . -j
```

常用配置：

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake .. -DUSE_GITEE_MIRROR=ON
cmake .. -DUSE_SYSTEM_JSON=ON
cmake .. -DJSON_LOCAL_PATH=/path/to/json
cmake .. -DBUILD_DEBUGGER=OFF
cmake .. -DCROSS_COMPILE_WINDOWS=ON
```

默认构建类型是 `Release`。构建输出目录由 CMake 配置为 `build/bin`、`build/lib` 等。

## 运行

当前 CLI 使用子命令形式：

```bash
./build/bin/utxo_Interpreter --help
./build/bin/utxo_Interpreter --version
./build/bin/utxo_Interpreter compile code.ct
./build/bin/utxo_Interpreter compile code.ct -d --debug-output code.debug
./build/bin/utxo_Interpreter run code.ct main 1
./build/bin/utxo_Interpreter ast code.ct main 1
./build/bin/utxo_Interpreter debug code.ct
./build/bin/utxo_Interpreter debug-server code.ct main 1
./build/bin/utxo_Interpreter shell code.ct
./build/bin/utxo_Interpreter compiler-repl
./build/bin/utxo_Interpreter test runtime
./build/bin/utxo_Interpreter test ast
```

全局选项包括：

- `-l, --log-level <debug|info|warning|error|critical|none>`
- `--asa` / `--allow-subscope-altstack`

`--asa` 会放宽 `setAlt` / `setMain` 在子作用域中的安全检查。不要在未阅读 `project_doc/STACK_MANAGEMENT_MECHANISM.md` 的情况下改变其默认语义。

## 测试

构建后优先运行：

```bash
ctest --test-dir build --output-on-failure
```

可单独运行的测试入口：

```bash
./build/bin/utxo_Interpreter test runtime
./build/bin/utxo_Interpreter test ast
bash test/repl/run_repl_smoke.sh ./build/bin/utxo_Interpreter
bash test/interpreter/run_ast_regression.sh
bash test/debugger_regression/run_debugger_regression.sh
bash test/compiler_regression/run_compiler_regression.sh
```

测试样例主要在 `test/` 下，包含 `.ct`、`.json`、`.txt` 和脚本。改动编译、解释、调试、REPL 或交易上下文相关逻辑时，应选择相应脚本或 `ctest` 做验证。

## 代码结构

- `main.cpp`：CLI 入口和子命令分发。
- `src/pass/`：PassManager、PassContext 和编译流水线基础设施。
- `src/lexer/`、`src/parser/`、`src/ast/`：源码导入、词法、语法和 AST。
- `src/compiler/`：前端流水线、符号收集、预分析、静态信息、AST 到 bytecode、常量折叠、库合并等。
- `src/bytecode/`：bytecode op、generator、scope、symtab、type validator、script decoder。
- `src/interpreter/`：AST interpreter、bytecode runner、runtime value、transaction context、builtins。
- `src/debugger/`：debugger core、BVM simulator、breakpoints、inspectors、CLI/debug server。
- `src/repl/` 和 `src/compiler_repl/`：运行 shell 和编译器 REPL。
- `src/config/`、`src/error/`、`src/log/`：配置、错误管理和日志。
- `stdlib/`：`.ct` 标准库模板。
- `doc/`、`docs/`、`project_doc/`：语言说明、用户文档和设计文档。

## 架构要点

典型编译路径：

1. 解析导入并词法分析。
2. 构建 AST。
3. 运行 AST visitors：符号收集、预分析、静态信息、bytecode 生成。
4. 输出 bytecode、debug info 或交给运行/调试入口继续处理。

变量和栈位置通过 `Scope` / `symtab` 建模。赋值、重命名、复合类型、alt stack、安全检查等逻辑需要结合 `project_doc/DESIGN_PHILOSOPHY.md` 和 `project_doc/STACK_MANAGEMENT_MECHANISM.md` 理解。

## 开发约定

- 遵循周围 C++20 代码风格和 `.clang-format`。
- 注释、错误信息、日志字符串优先沿用周围语言风格；本仓库大量使用中文注释和日志。
- 新增 `.cpp` 文件时，同步加入 `CMakeLists.txt` 的对应源文件列表：
  - 通用核心逻辑加入 `CORE_SOURCES`
  - compiler REPL 逻辑加入 `COMPILER_REPL_SOURCES`
  - debugger 逻辑加入 `DEBUGGER_SOURCES`，并保持受 `BUILD_DEBUGGER` 控制
- 修改 CLI 时同步检查 `--help` 输出和相关示例。
- 修改语言语法、内建函数、内建对象或运行语义时，同步检查 `doc/`、`docs/`、`project_doc/` 中相关文档是否需要更新。
- 修改 stdlib 查找、安装或模板逻辑时，注意 `APC_STDLIB_PATH`、`user_preferences.json`、`APC_DEFAULT_STDLIB_PATH` 和本地 `./stdlib` 的查找顺序。
- 不要把构建产物、临时日志或大体积生成文件作为源码改动提交，除非任务明确要求。
