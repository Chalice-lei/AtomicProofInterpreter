# AtomicProof Compiler 项目代码走读

这份文档按“结对读代码”的方式理解整个项目：先看全景，再看目录，再沿着启动命令、Pass 流水线、核心模块、调用链和数据流一步步往里走。

说明：仓库根目录没有发现传统的 `README.md`。本文结合了 `CLAUDE.md`、`docs/zh/overview.md`、`docs/zh/installation.md`、`doc/GRAMMAR_SPECIFICATION.md`、`CMakeLists.txt`、`main.cpp`、`src/` 和 `test/` 下的实际代码来整理。

## 一、项目全景图

### 1. 这个项目是做什么的？

这个项目是一个 C++ 编写的 UTXO 智能合约编译器，可执行产物名在 `CMakeLists.txt` 中是 `utxo_interpreter`。

它把一种扩展名为 `.ct` 的高级合约语言编译成 Bitcoin Script / BVM 可执行的脚本字节码，并导出 JSON 文件。合约语言风格接近 Python，支持 `Contract`、`Library`、`Struct`、函数、变量声明、条件、循环、内置函数和导入标准库。

关键证据：

- `CMakeLists.txt`：定义项目构建，C++20，可执行文件输出到 `build/bin/utxo_interpreter`。
- `main.cpp`：命令行入口，读取 `.ct` 文件，运行 Lexer、Parser、AST 转字节码、优化、导出结果等 Pass。
- `docs/zh/overview.md`：说明编译流程是源代码到词法分析、语法分析、语义分析、字节码生成、JSON 输出。
- `doc/GRAMMAR_SPECIFICATION.md`：描述 `.ct` 合约语言语法。

### 2. 它主要解决什么问题？

它解决的是：开发者不直接手写底层 Bitcoin Script，而是用较高层、可读性更好的 `.ct` 合约语言描述 UTXO 合约逻辑，再由编译器生成底层脚本。

这个项目还额外解决了几个工程问题：

- 把源代码拆成 Token、AST、Bytecode 等阶段，降低编译复杂度。
- 通过 `PreAnalysisVisitor` 做变量所有权和栈安全检查，避免栈值被错误重复使用或被消费后继续使用。
- 通过 `BytecodePeepholePass` 和 `BytecodeFinalizePass` 做字节码优化和最终填充。
- 通过调试器相关模块支持交互式 CLI 调试。

### 3. 使用了哪些语言、框架、库和工具？

主要语言：

- C++20：核心编译器、调试器、字节码生成器。
- Shell：构建、回归测试、发布脚本。
- `.ct`：项目自己的合约语言和标准库源码。

构建工具：

- CMake：见 `CMakeLists.txt`。
- Make / Ninja 等 CMake 后端：实际由用户环境决定。

第三方库：

- `nlohmann/json`：JSON 解析与生成，在 `CMakeLists.txt` 中通过系统库、GitHub / Gitee FetchContent 或本地路径引入。

项目内部工具：

- `Logger`：见 `src/log/logger.h` 和 `src/log/logger.cpp`。
- `ErrorManager`：见 `src/error/error_manager.h` 和 `src/error/error_manager.cpp`。
- `ConfigManager`：见 `src/config/config_manager.h` 和 `src/config/config_manager.cpp`。
- Pass 框架：见 `src/pass/pass.h`、`src/pass/pass_context.h`、`src/pass/pass_manager.cpp`。

测试 / 脚本：

- 编译回归测试：`test/compiler_regression/run_compiler_regression.sh`。
- 调试器回归测试：`test/debugger_regression/run_debugger_regression.sh`。
- 跨平台构建：`scripts/cross-platform-builder.sh`。
- 发布打包：`scripts/release-packager.sh`。

### 4. 项目的核心输入是什么？

核心输入是一个 `.ct` 合约源文件。

例如：

- `wallet.ct`
- `test/contract_file/counter.ct`
- `test/debugger_regression/debug_line_mapping_basic.ct`

命令行参数也是输入的一部分，主要由 `main.cpp` 的 `parseCommandLineArgs(int argc, char* argv[])` 解析：

- `.ct` 文件路径。
- `--allow-subscope-altstack` 或 `--asa`。
- `--debug`。
- `-d`。
- `--debug-output` / `--debug-out`。
- `--log-level`。

配置文件也是辅助输入：

- `user_preferences.json`：由 `ConfigManager::initialize("user_preferences.json")` 读取。

标准库也是编译输入的一部分：

- `stdlib/`：例如 `stdlib/std/p2pkh.ct`。
- 导入解析由 `ImportResolver::resolveWithMap` 处理。

### 5. 项目的核心输出是什么？

核心输出是一个 JSON 文件，文件名一般是输入 `.ct` 文件的 stem 加 `.json`，由 `ExportResultsPass::execute` 写出。

例如输入：

```text
test/debugger_regression/debug_line_mapping_basic.ct
```

如果在某个目录执行编译，输出通常是：

```text
debug_line_mapping_basic.json
```

JSON 里主要包含：

- `metadata`：编译器信息、构建信息、源文件信息，来自 `ConfigManager::generateMetadata`。
- `structs`：结构体定义，来自 `StaticInfoVisitor`。
- `abi`：公开函数 ABI，来自 `StaticInfoVisitor`。
- `lock.asm`：可读汇编格式脚本。
- `lock.hex`：最终脚本十六进制字节码。
- `unlock`：每个公开函数的解锁参数模板。
- `constructorParams`：构造参数信息。
- `functions`：函数信息。

如果启用调试信息，还可能输出 `.debug` 或用户指定的 debug JSON 文件。

### 6. 用一句话总结这个项目的运行模型

这个项目的运行模型是：命令行读取 `.ct` 源文件，经过导入展开、词法分析、语法分析、静态分析、AST 到字节码转换、字节码优化、最终填充和 JSON 导出，生成可供 BVM / Bitcoin Script 执行的合约脚本。

## 二、项目目录结构

### 根目录

```text
文件/目录：main.cpp
作用：编译器命令行入口。
为什么重要：所有普通编译和调试模式都会从这里开始。
和其他模块的关系：调用 ConfigManager、Logger、ErrorManager，构造 PassContext，注册并运行 LexerPass、ParserPass、ASTToBytecodePass、BytecodePeepholePass、BytecodeFinalizePass、ExportResultsPass。
```

```text
文件/目录：CMakeLists.txt
作用：项目构建配置。
为什么重要：定义 C++ 标准、可执行文件、源码列表、nlohmann/json 依赖、调试器开关、安装规则和编译宏。
和其他模块的关系：把 main.cpp、src/ 下的编译器源码、可选 debugger 源码编译成 build/bin/utxo_interpreter。
```

```text
文件/目录：CLAUDE.md
作用：项目给代码助手看的架构说明。
为什么重要：它简明列出了构建命令、主要流程、Pass 顺序和关键目录。
和其他模块的关系：和 main.cpp、CMakeLists.txt、src/ 实际结构基本对应，是快速理解项目的入口文档。
```

```text
文件/目录：user_preferences.json
作用：用户偏好配置文件。
为什么重要：main.cpp 会调用 ConfigManager::initialize("user_preferences.json") 尝试加载它。
和其他模块的关系：ConfigManager 提供 getConfigValue，ImportResolver 会读取 paths.stdlib 来寻找标准库路径；当前文件里没有发现 paths.stdlib，所以标准库路径主要靠环境变量、安装路径或 cwd/stdlib。
```

```text
文件/目录：wallet.ct、schnorr_demo.ct、test_*.ct
作用：示例或测试用合约源文件。
为什么重要：它们展示 .ct 语言的实际写法。
和其他模块的关系：可以作为 utxo_interpreter 的输入，经过完整编译流程生成 JSON。
```

### src/ 核心源码目录

```text
文件/目录：src/pass/
作用：Pass 框架。
为什么重要：项目的编译过程不是一个大函数完成，而是拆成多个 Pass 串起来执行。
和其他模块的关系：main.cpp 注册 Pass；PassManager 负责依赖排序和执行；PassContext 在 Pass 之间传递 tokens、ast、bytecode、abi 等数据。
```

关键文件：

- `src/pass/pass.h`：定义抽象类 `Pass`。
- `src/pass/pass_context.h`：定义 `PassContext`，用字符串 key 和类型保存跨 Pass 数据。
- `src/pass/pass_manager.cpp`：实现 `PassManager::registerPass`、`PassManager::enablePass`、`PassManager::run`。
- `src/pass/pass_macros.h`：提供 `REGISTER_PASS` 和 `DECLARE_PASS` 宏。

```text
文件/目录：src/lexer/
作用：导入解析、源码预处理、词法分析。
为什么重要：它是源代码进入编译器后的第一道真正处理。
和其他模块的关系：LexerPass 调用 ImportResolver 和 Lexer，输出 Token 列表给 ParserPass。
```

关键文件：

- `src/lexer/import_resolver.h` / `src/lexer/import_resolver.cpp`：处理 `import std.xxx` 和相对路径 import。
- `src/lexer/lexer.h` / `src/lexer/lexer.cpp`：把源码文本扫描成 Token。
- `src/lexer/import_resolver.h` / `src/lexer/import_resolver.cpp`：解析 import，并保留源码行映射。

```text
文件/目录：src/parser/
作用：语法分析。
为什么重要：它把 Token 序列转成 AST。
和其他模块的关系：ParserPass 调用 Parser::parseContract，输出 ContractNode 给 ASTToBytecodePass。
```

关键文件：

- `src/parser/parser.h` / `src/parser/parser.cpp`：实现 `Parser::parseContract`、`Parser::parseFunction`、`Parser::parseStatement`、`Parser::parseExpression` 等语法解析函数。

```text
文件/目录：src/ast/
作用：AST 节点定义和源码位置映射。
为什么重要：AST 是编译器中间表示，后续静态分析和代码生成都围绕 AST 进行。
和其他模块的关系：Parser 创建 AST；compiler 目录中的 Visitor 遍历 AST；source_location_mapper 把 import 展开后的行号映射回原始文件。
```

关键文件：

- `src/ast/ast.h` / `src/ast/ast.cpp`：定义 `ContractNode`、`FunctionNode`、`StructDefNode`、`IfNode`、`ReturnNode`、`LiteralNode` 等。
- `src/ast/visitor.h`：Visitor 接口。
- `src/ast/source_location_mapper.h`：处理源码位置映射。

```text
文件/目录：src/compiler/
作用：语义分析、静态信息收集、常量折叠、AST 到字节码转换。
为什么重要：这里是核心业务逻辑最密集的地方。
和其他模块的关系：ASTToBytecodePass 调用 ASTToBytecodeConverter，Converter 再依次调用多个 Visitor。
```

关键文件：

- `src/compiler/ast_to_bytecode_converter.h`：`ASTToBytecodeConverter::convert` 是 AST 到字节码转换的总入口。
- `src/compiler/collect_symbols_visitor.h`：`CollectSymbolsVisitor` 检查函数、结构体、变量名冲突。
- `src/compiler/static_info_visitor.h`：`StaticInfoVisitor` 生成 ABI、unlock、constructorParams、structs、functions。
- `src/compiler/pre_analysis_visitor.h` / `.cpp`：`PreAnalysisVisitor` 做变量所有权、消费关系、栈安全和静态循环分析。
- `src/compiler/constant_folder.h`：`ConstantFolder` 做常量折叠和简单优化。
- `src/compiler/ast_to_bytecode_visitor.h` / `.cpp`：`ASTToBytecodeVisitor` 负责真正生成字节码。

```text
文件/目录：src/bytecode/
作用：字节码、操作码、栈模型、符号表和内置函数。
为什么重要：这是高级语法最终落到脚本指令的地方。
和其他模块的关系：ASTToBytecodeVisitor 通过 BytecodeGenerator、Scope、SymbolTable、BuiltinFunctionFactory、OpFunctionFactory 生成最终脚本。
```

关键文件：

- `src/bytecode/bytecode_generator.h` / `.cpp`：`BytecodeGenerator::emit` 生成指令。
- `src/bytecode/bytecode_opcodes.h`：定义 `BytOpcode` 和 opcode 到 hex 的映射。
- `src/bytecode/bytecode_operation_functions.h`：普通内置操作函数工厂，例如 `Hash160`、`CheckSig`、`EqualVerify`。
- `src/bytecode/bytecode_builtin_function.h`：特殊内置函数工厂，例如 `SetAlt`、`SetMain`、`Clone`、`Range`、`Move`、`Keep`。
- `src/bytecode/bytecode_builtin_struct.h`：内置对象，例如 `self` 和 `BVM`。
- `src/bytecode/scope.h` / `.cpp`：面向代码生成的作用域和栈操作封装。
- `src/bytecode/symtab.h` / `.cpp`：符号表和主栈、altstack、fixed area 状态。

```text
文件/目录：src/error/
作用：错误管理。
为什么重要：词法、语法、语义、类型错误都通过它报告。
和其他模块的关系：Lexer、Parser、Compiler Visitor、Pass 都可能调用 ErrorManager 或相关错误宏。
```

关键文件：

- `src/error/error_manager.h` / `.cpp`：`ErrorManager::reportError`、`ErrorManager::hasErrors`、`ErrorManager::printAllErrors`。
- 错误宏：`LEXICAL_ERROR`、`SYNTAX_ERROR`、`SEMANTIC_ERROR`、`TYPE_ERROR` 等。

```text
文件/目录：src/log/
作用：日志。
为什么重要：编译过程大量使用 LOG_DEBUG、LOG_INFO、LOG_ERROR 等宏。
和其他模块的关系：main.cpp 初始化 Logger；各模块输出调试信息。
```

关键文件：

- `src/log/logger.h` / `.cpp`：`Logger::initialize`、`Logger::shutdown`。

```text
文件/目录：src/config/
作用：配置和编译器信息。
为什么重要：读取用户配置，生成 metadata，提供版本、构建类型、git hash 等信息。
和其他模块的关系：main.cpp 初始化 ConfigManager；ExportResultsPass 通过 ConfigManager 生成 metadata；ImportResolver 通过 ConfigManager 找 stdlib 路径。
```

关键文件：

- `src/config/config_manager.h` / `.cpp`：`ConfigManager::initialize`、`ConfigManager::generateMetadata`。
- `src/config/compiler_info.h`：编译期宏封装。

```text
文件/目录：src/debugger/
作用：集成 CLI 调试器和 BVM 模拟器。
为什么重要：启用 BUILD_DEBUGGER 后，用户可以用 --debug 进入交互式调试。
和其他模块的关系：main.cpp 的 startDebugger 调用 DebuggerCore::compileSource，再交给 BVMSimulator 和 CLIDebugger。
```

关键文件：

- `src/debugger/core/debugger_core.cpp`：`DebuggerCore::compileSource` 会用同一套 Pass 编译源码并生成调试信息。
- `src/debugger/interface/cli_debugger.h`：交互式命令入口。
- `src/debugger/vm/bvm_simulator.*`：执行脚本指令的模拟器。

### stdlib/ 标准库目录

```text
文件/目录：stdlib/
作用：.ct 标准库。
为什么重要：用户合约可以 import 标准库函数，避免重复写常见脚本逻辑。
和其他模块的关系：ImportResolver 根据 import 语句找到 stdlib 下的 .ct 文件并展开到编译输入中。
```

典型文件：

- `stdlib/std/p2pkh.ct`：定义 `Library std.p2pkh` 和 `verifyP2PKH`。

### docs/ 与 doc/

```text
文件/目录：docs/zh/
作用：面向用户的中文说明文档。
为什么重要：解释项目概念、安装、测试方式。
和其他模块的关系：帮助理解代码意图，但不是编译时依赖。
```

```text
文件/目录：doc/
作用：语法、内置函数、内置对象、学习计划等文档。
为什么重要：解释 .ct 语言和内置 API。
和其他模块的关系：doc/GRAMMAR_SPECIFICATION.md 对应 Parser 的实现；doc/BUILTIN_FUNCTION_DOC.md 对应 bytecode_operation_functions 和 bytecode_builtin_function 中的函数工厂。
```

### test/ 测试目录

```text
文件/目录：test/compiler_regression/
作用：编译器回归测试。
为什么重要：它通过编译临时 .ct 用例验证成功、失败、错误信息等行为。
和其他模块的关系：调用 build/bin/utxo_interpreter，覆盖 Lexer、Parser、PreAnalysis、Codegen、Export 等完整流程。
```

关键文件：

- `test/compiler_regression/run_compiler_regression.sh`

```text
文件/目录：test/debugger_regression/
作用：调试器回归测试。
为什么重要：验证 debug info、PC 映射、CLI 调试命令。
和其他模块的关系：调用编译器的 debug 输出能力和 debugger 模块。
```

关键文件：

- `test/debugger_regression/run_debugger_regression.sh`
- `test/debugger_regression/debug_line_mapping_basic.ct`

### scripts/、docker/、.github/

```text
文件/目录：scripts/
作用：跨平台构建、发布打包、快速发布。
为什么重要：面向构建和交付。
和其他模块的关系：调用 CMake 构建 utxo_interpreter，并把 bin 和 stdlib 打包。
```

```text
文件/目录：docker/
作用：Docker 构建环境。
为什么重要：提供统一的 Ubuntu 22.04 跨平台构建环境。
和其他模块的关系：运行 scripts/cross-platform-builder.sh 或发布相关脚本。
```

```text
文件/目录：.github/workflows/
作用：GitHub Actions 工作流。
为什么重要：对 push / PR 做变更检测、构建和通知。
和其他模块的关系：代码变更时会安装编译器依赖并运行构建脚本。
```

## 三、项目启动流程

这里的“启动”指启动编译器去编译一个 `.ct` 文件。

### 第 1 步：构建项目

文档和 `CLAUDE.md` 中给出的典型构建方式是：

```bash
mkdir build
cd build
cmake ..
cmake --build . -j
```

构建配置来自：

- `CMakeLists.txt`

构建产物通常是：

```text
build/bin/utxo_interpreter
```

### 第 2 步：用户执行编译命令

典型命令：

```bash
./build/bin/utxo_interpreter test/contract_file/counter.ct
```

或者在 `build/` 目录内：

```bash
./bin/utxo_interpreter ../test/contract_file/counter.ct
```

调试模式：

```bash
./build/bin/utxo_interpreter --debug test/contract_file/counter.ct
```

输出 debug 信息：

```bash
./build/bin/utxo_interpreter -d test/contract_file/counter.ct
./build/bin/utxo_interpreter --debug-output counter.debug.json test/contract_file/counter.ct
```

### 第 3 步：命令进入 main.cpp

入口文件：

- `main.cpp`

入口函数：

- `int main(int argc, char* argv[])`

第一件重要的事是解析命令行参数：

- `parseCommandLineArgs(int argc, char* argv[])`

这个函数会识别：

- `-h` / `--help` / `help`
- `-v` / `--version`
- `-l` / `--log-level`
- `--allow-subscope-altstack` / `--asa`
- `--debug`
- `-d`
- `--debug-output` / `--debug-out`
- `--stack-trace-output`
- 输入 `.ct` 文件

它还会检查输入文件扩展名必须是 `.ct`。

### 第 4 步：初始化日志和配置

`main.cpp` 会初始化日志：

- `Logger::getInstance().initialize(...)`

然后初始化配置：

- `ConfigManager::getInstance().initialize("user_preferences.json")`

配置加载失败不一定阻止编译，因为 `ConfigManager` 对缺失配置有容忍逻辑。

### 第 5 步：读取源文件

`main.cpp` 会检查：

- 文件是否存在。
- 是否是普通文件。
- 是否为空。

然后以二进制方式读入字符串：

```cpp
std::string tbc_file((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
```

这个字符串后面会放到 `PassContext`，key 是：

```text
source_code
```

### 第 6 步：如果是调试模式，进入 debugger 流程

如果命令行带 `--debug`，`main.cpp` 会走：

- `startDebugger(args.filename, tbc_file)`

这个函数只在启用 `ENABLE_DEBUGGER` 时可用。它大致会做：

- 设置错误管理器。
- 选择语言。
- 调用 `DebuggerCore::compileSource` 编译源码并生成 debug info。
- 创建 `BVMSimulator`。
- 创建 `BreakpointManager`。
- 读取用户选择的函数和参数。
- 启动 `CLIDebugger::run`。

调试流程和普通编译共用核心 Pass，只是增加了 debug 信息和交互执行。

### 第 6.1 步：生成栈变化可视化 trace

如果想在浏览器里看每条字节码执行前后的主栈和 altstack 变化，可以使用 `run` 子命令导出栈 trace：

```bash
./build/bin/utxo_interpreter run test/debugger_regression/debug_stack_visualizer_alt.ct test_alt_roundtrip 5 --stack-trace-output stack_trace.json
```

生成的 `stack_trace.json` 包含：

- step index、PC、opcode、operand。
- source file、source line、function name。
- main stack before / after。
- alt stack before / after。
- pushed、popped、main <-> alt moved 元素摘要。

推荐在 VS Code 里打开：

```text
tools/vscode_stack_visualizer
```

用 VS Code 的 `Developer: Install Extension from Location...` 选择该目录，或作为扩展开发目录运行，然后执行命令：

```text
AtomicProof: Visualize Active Stack Trace
```

也可以直接执行：

```text
AtomicProof: Generate Stack Trace and Visualize
```

让扩展调用 `build/bin/utxo_interpreter` 生成 trace 并自动打开 Webview。

Webview 左侧显示源码并高亮当前行，中间显示当前 step / PC / 指令和完整 JSON，右侧用 Top 在上方的两个竖向栈展示 Main Stack 和 Alt Stack。Diff 模式会用绿色标出 push、红色标出 pop/drop、黄色或蓝色标出 main stack 和 altstack 之间的移动。

### 第 7 步：普通编译模式创建 PassContext

普通模式下，`main.cpp` 创建：

- `PassContext pipelineData`

并写入关键数据：

- `code_file_name`：输入文件 stem。
- `source_file_path`：输入文件路径。
- `source_code`：源代码内容。
- `allow_subscope_altstack`：是否允许子作用域 altstack。
- `enable_debug`：是否输出 debug 信息。
- `debug_output_file`：用户指定的 debug 输出文件，可选。

这些 key 会在后续 Pass 中被读取。

### 第 8 步：注册并启用编译 Pass

`main.cpp` 使用宏注册 Pass：

- `LexerPass`
- `ParserPass`
- `ASTToBytecodePass`
- `BytecodePeepholePass`
- `BytecodeFinalizePass`
- `ExportResultsPass`

对应文件：

- `src/lexer_pass.h`
- `src/parser_pass.h`
- `src/ast_to_bytecode_pass.h`
- `src/bytecode_peephole_pass.h`
- `src/bytecode_finalize_pass.h`
- `src/export_results_pass.h`

### 第 9 步：PassManager 运行流水线

调用点：

- `pm.run(pipelineData)`

实现位置：

- `src/pass/pass_manager.cpp`

核心函数：

- `PassManager::run(PassContext& context)`
- `PassManager::resolveDependencies(...)`
- `PassManager::executePass(...)`

Pass 会按依赖顺序执行：

```text
LexerPass
-> ParserPass
-> ASTToBytecodePass
-> BytecodePeepholePass
-> BytecodeFinalizePass
-> ExportResultsPass
```

### 第 10 步：LexerPass 展开 import 并生成 Token

文件：

- `src/lexer_pass.h`

函数：

- `LexerPass::execute(PassContext& context)`

做的事：

1. 从 `PassContext` 取 `source_code` 和 `source_file_path`。
2. 创建 `ImportResolver`。
3. 调用 `ImportResolver::resolveWithMap` 展开标准库和相对 import。
4. 设置 `ErrorManager` 的 SourceMap。
5. 创建 `Lexer`。
6. 调用 `Lexer::tokenize` 生成 token 列表。
7. 把 token 列表写回 `PassContext`，key 是 `tokens`。

### 第 11 步：ParserPass 生成 AST

文件：

- `src/parser_pass.h`

函数：

- `ParserPass::execute(PassContext& context)`

做的事：

1. 从 `PassContext` 取 `tokens`。
2. 创建 `Parser`。
3. 调用 `Parser::parseContract`。
4. 对 AST 应用源码映射。
5. 把 AST 写回 `PassContext`，key 是 `ast`。

### 第 12 步：ASTToBytecodePass 做核心编译

文件：

- `src/ast_to_bytecode_pass.h`

函数：

- `ASTToBytecodePass::execute(PassContext& context)`

做的事：

1. 从 `PassContext` 取 `ast`。
2. 把 `ContractNode::libraries` 中的库函数合并到 `members`。
3. 创建 `ASTToBytecodeConverter`。
4. 调用 `ASTToBytecodeConverter::convert`。
5. 收集 ABI、unlock、constructorParams、structs、functions。
6. 写回 `PassContext`。

重要细节：

- 字节码数据写入 key 名是 `"bytcode"`，这是当前代码里的拼写，不是 `"bytecode"`。

### 第 13 步：BytecodePeepholePass 做局部优化

文件：

- `src/bytecode_peephole_pass.h`

函数：

- `BytecodePeepholePass::execute(PassContext& context)`
- `BytecodePeepholePass::runOnePass(...)`

它会优化一些相邻指令模式，例如：

- `OP_DUP OP_DROP` 删除。
- `OP_SWAP OP_SWAP` 删除。
- `OP_DROP OP_DROP` 合并成 `OP_2DROP`。
- `OP_NOT OP_NOT` 合并成 `OP_0NOTEQUAL`。

### 第 14 步：BytecodeFinalizePass 做最终填充

文件：

- `src/bytecode_finalize_pass.h`

函数：

- `BytecodeFinalizePass::execute(PassContext& context)`

它会：

1. 找到最后一个 `OP_RETURN`。
2. 计算有效脚本长度。
3. 用 `OP_INVALIDOPCODE` 填充，使脚本按 64 字节对齐。
4. 保留必要的后缀。

### 第 15 步：ExportResultsPass 导出 JSON

文件：

- `src/export_results_pass.h`

函数：

- `ExportResultsPass::execute(PassContext& context)`

它会：

1. 从 `PassContext` 取 `"bytcode"`。
2. 拼接指令 hex。
3. 调用 `tbc::script_decoder::hex_to_asm` 生成 asm。
4. 调用 `ConfigManager::generateMetadata` 生成 metadata。
5. 写出 `<code_file_name>.json`。

## 四、深入讲核心代码逻辑

### 1. main.cpp：命令行和总控入口

负责什么：

- 解析命令行。
- 初始化日志、配置、错误管理。
- 读取 `.ct` 文件。
- 根据参数选择普通编译或调试模式。
- 创建并运行 Pass 流水线。

主要函数：

- `parseCommandLineArgs(int argc, char* argv[])`
- `main(int argc, char* argv[])`
- `startDebugger(...)`
- `promptForFunctionParameters(...)`
- `parseOneInput(...)`

输入：

- 命令行参数。
- `.ct` 文件路径。
- 可选配置文件。

关键处理：

- `parseCommandLineArgs` 校验 `.ct` 扩展名。
- `main` 读取源码，填充 `PassContext`。
- 普通模式调用 `PassManager::run`。
- 调试模式调用 `startDebugger`。

输出：

- 普通模式输出 JSON。
- 调试模式进入 CLI 调试器。
- 出错时返回非零状态码。

调用其他模块：

- `Logger`
- `ConfigManager`
- `ErrorManager`
- `PassManager`
- 各个 Pass
- debugger 模块

理解重点：

- `main.cpp` 本身不做编译细节，只负责把输入放进 `PassContext`，再让 Pass 流水线工作。

### 2. Pass 框架：编译流水线骨架

负责什么：

- 管理编译阶段。
- 解决 Pass 依赖。
- 按顺序执行 Pass。
- 让不同 Pass 通过 `PassContext` 交换数据。

主要文件和函数：

- `src/pass/pass.h`：`class Pass`
- `src/pass/pass_context.h`：`PassContext::set`、`PassContext::get`、`PassContext::tryGet`
- `src/pass/pass_manager.cpp`：`PassManager::registerPass`、`PassManager::enablePass`、`PassManager::run`

输入：

- 已注册的 Pass。
- `PassContext`。

关键处理：

- 根据 `getDependencies()` 解析依赖。
- 每个 Pass 执行 `initialize`、`execute`、`finalize`。

输出：

- 被不断补充的 `PassContext`。

调用其他模块：

- 不直接知道 Lexer、Parser 细节，只调用 Pass 抽象接口。

理解重点：

- 这是项目的“流水线发动机”。
- 想知道编译阶段顺序，看 Pass 的依赖关系最直接。

### 3. LexerPass / ImportResolver / Lexer：源码到 Token

负责什么：

- 展开 import。
- 建立源码映射。
- 把字符流扫描为 Token。

主要文件和函数：

- `src/lexer_pass.h`：`LexerPass::execute`
- `src/lexer/import_resolver.cpp`：`ImportResolver::resolveWithMap`
- `src/lexer/lexer.cpp`：`Lexer::tokenize`、`Lexer::scanToken`、`Lexer::handleIndentation`、`Lexer::readAlphaNumeric`、`Lexer::readString`

输入：

- `source_code`
- `source_file_path`

关键处理：

- `ImportResolver::resolveWithMap` 支持：
  - `import std.p2pkh`
  - `import "./relative.ct"`
- `ImportResolver` 会避免重复 import，并处理循环 import。
- `Lexer::tokenize` 扫描源码，生成 Token。
- `Lexer::handleIndentation` 生成类似 Python 的 `INDENT` / `DEDENT`。
- `Lexer::readAlphaNumeric` 识别关键字、类型、数字、标识符。
- `Lexer::readString` 识别字符串，也会把部分比特币地址识别成地址 Token。

输出：

- `std::vector<Token>`
- SourceMap

调用其他模块：

- `ErrorManager`
- `ConfigManager`

理解重点：

- import 展开发生在词法分析之前。
- 源码位置映射很重要，因为错误信息和调试信息需要映射回原文件。

### 4. Parser：Token 到 AST

负责什么：

- 把 Token 序列解析成 AST。

主要文件和函数：

- `src/parser_pass.h`：`ParserPass::execute`
- `src/parser/parser.cpp`：`Parser::parseContract`
- `src/parser/parser.cpp`：`Parser::parseLibrary`
- `src/parser/parser.cpp`：`Parser::parseFunction`
- `src/parser/parser.cpp`：`Parser::parseStatement`
- `src/parser/parser.cpp`：`Parser::parseExpression`
- `src/parser/parser.cpp`：`Parser::parsePrimary`

输入：

- Token 列表。

关键处理：

- `Parser::parseContract` 要求最终有一个 `Contract`。
- `Parser::parseLibrary` 支持库定义，但库必须在合约之前。
- `Parser::parseFunction` 解析函数名、参数、返回类型和函数体。
- `Parser::parseStatement` 分派解析 if、for、return、变量声明、赋值、表达式语句等。
- `Parser::parseExpression` 解析表达式和运算符优先级。

输出：

- `ContractNode`

调用其他模块：

- AST 节点类。
- `ErrorManager`。

理解重点：

- 这个项目的 AST 类型定义在 `src/ast/ast.h`。
- 大写 `Return(...)` 和小写 `return ...` 语义不同：
  - 大写 `Return` 会生成脚本里的 `OP_RETURN`。
  - 小写 `return` 用于返回值传递，常见于私有函数或库函数。

### 5. AST 节点：编译器的中间表示

负责什么：

- 表达合约代码的结构。

主要文件：

- `src/ast/ast.h`
- `src/ast/ast.cpp`
- `src/ast/visitor.h`

主要节点：

- `ContractNode`
- `LibraryNode`
- `FunctionNode`
- `ConstructorNode`
- `StructDefNode`
- `BlockNode`
- `IfNode`
- `ForNode`
- `AssignNode`
- `VarDeclNode`
- `ReturnNode`
- `LiteralNode`
- `IdentifierNode`
- `CallNode`
- `MethodCallNode`
- `OpNode`
- `FieldAccessNode`
- `IndexAccessNode`

输入：

- Parser 根据 Token 创建 AST。

关键处理：

- AST 自身主要保存结构。
- 实际语义由 Visitor 完成。

输出：

- 后续 Visitor 遍历的树结构。

调用其他模块：

- Parser 创建 AST。
- Compiler Visitor 读取 AST。

理解重点：

- 读 AST 时要先分清“语句节点”和“表达式节点”。
- 后续很多编译逻辑都是 `visit(XxxNode&)`。

### 6. ASTToBytecodeConverter：核心编译总入口

负责什么：

- 把 AST 转成字节码的总协调器。

文件：

- `src/compiler/ast_to_bytecode_converter.h`

主要函数：

- `ASTToBytecodeConverter::convert(ContractNode& ast, bool allowSubscopeAltstack)`

输入：

- `ContractNode`
- `allowSubscopeAltstack`

关键处理顺序：

1. `CollectSymbolsVisitor::checkUniqueness`
2. `StaticInfoVisitor::visit`
3. `PreAnalysisVisitor::analyze`
4. `ConstantFolder::fold`
5. `ASTToBytecodeVisitor::visit`

输出：

- `std::pair<std::vector<std::string>, std::unordered_map<std::string, std::string>>`

其中：

- vector 是生成的字节码指令。
- map 是 unlock 模板。

调用其他模块：

- `CollectSymbolsVisitor`
- `StaticInfoVisitor`
- `PreAnalysisVisitor`
- `ConstantFolder`
- `ASTToBytecodeVisitor`
- `BytecodeGenerator`

理解重点：

- 这里是从 AST 到 Bytecode 的核心编译阶段入口。
- 代码生成之前会先做静态检查和常量折叠。

### 7. CollectSymbolsVisitor：符号唯一性检查

负责什么：

- 检查函数、结构体、变量名是否冲突。
- 收集结构体定义。

文件：

- `src/compiler/collect_symbols_visitor.h`

主要函数 / 方法：

- `CollectSymbolsVisitor::checkUniqueness`
- `CollectSymbolsVisitor::visit(ContractNode&)`
- `CollectSymbolsVisitor::visit(FunctionNode&)`
- `CollectSymbolsVisitor::visit(StructDefNode&)`

输入：

- AST。

关键处理：

- 检查重复函数。
- 检查重复结构体。
- 检查变量名是否和结构体名冲突。

输出：

- 结构体定义集合。
- 错误信息。

调用其他模块：

- AST Visitor。
- `ErrorManager`。

理解重点：

- 它还不做复杂类型推断，主要是符号层面的基础检查。

### 8. StaticInfoVisitor：生成 ABI 和静态元信息

负责什么：

- 生成导出 JSON 中的 ABI、unlock、constructorParams、structs、functions。

文件：

- `src/compiler/static_info_visitor.h`

主要函数：

- `StaticInfoVisitor::visit(ContractNode&)`
- `StaticInfoVisitor::visit(FunctionNode&)`
- `StaticInfoVisitor::visit(ConstructorNode&)`
- `StaticInfoVisitor::visit(StructDefNode&)`

输入：

- AST。

关键处理：

- 公开函数加入 ABI。
- 以下函数不作为公开 ABI：
  - 名字以下划线开头的函数。
  - 来自 Library 的函数。
- 函数参数写入 ABI。
- `unlock` 模板按参数生成，例如 `<signature><pubKey>`。
- 构造参数写入 `constructorParams`。
- 结构体写入 `structs`。

输出：

- `json abi`
- `json unlock`
- `json constructorParams`
- `json structs`
- `json allFunctions`

调用其他模块：

- AST Visitor。
- nlohmann/json。

理解重点：

- ABI 不是从最终字节码反推的，而是在 AST 阶段根据函数定义生成的。

### 9. PreAnalysisVisitor：所有权和栈安全检查

负责什么：

- 分析变量是否被正确使用。
- 检查变量是否被消费后再次使用。
- 处理 `.Clone()`、`Move`、`SetAlt`、`SetMain` 等特殊语义。
- 分析静态 `Range` 循环。

文件：

- `src/compiler/pre_analysis_visitor.h`
- `src/compiler/pre_analysis_visitor.cpp`

主要函数：

- `PreAnalysisVisitor::analyze`
- `PreAnalysisVisitor::visit(FunctionNode&)`
- `PreAnalysisVisitor::visit(IfNode&)`
- `PreAnalysisVisitor::visit(ForNode&)`
- `PreAnalysisVisitor::visit(CallNode&)`
- `PreAnalysisVisitor::visit(MethodCallNode&)`
- `PreAnalysisVisitor::useVariable`
- `PreAnalysisVisitor::consumeVariable`

输入：

- AST。
- `allowSubscopeAltstack`。

关键处理：

- 函数参数声明为变量。
- 变量使用后状态变化。
- 一些内置函数会消费参数，例如 `Hash160`、`CheckSig`、`Sha256`。
- `.Clone()` 表示复制，避免直接消费原变量。
- `ForNode` 目前主要支持 `Range(...)` 的静态循环，并记录静态迭代值。
- `IfNode` 会分别分析两个分支并合并变量状态。

输出：

- 静态检查通过 / 失败。
- For 循环静态展开信息。
- 错误信息。

调用其他模块：

- AST。
- `ErrorManager`。

理解重点：

- 这是理解项目的关键模块之一。
- 它的思想很像：栈上的值很多时候只能“移动”一次，除非显式 Clone。
- 如果后面代码生成报奇怪的栈问题，通常要先回来看这里。

### 10. ConstantFolder：常量折叠

负责什么：

- 在代码生成前做一些 AST 级别优化。

文件：

- `src/compiler/constant_folder.h`

主要函数：

- `ConstantFolder::fold`

输入：

- AST。

关键处理：

- 折叠纯数字表达式。
- 简单常量传播。
- 删除永远不会执行的分支或循环。
- 处理除零错误。

输出：

- 被改写或优化过的 AST。

调用其他模块：

- AST。
- `ErrorManager`。

理解重点：

- 它发生在 `PreAnalysisVisitor` 之后、`ASTToBytecodeVisitor` 之前。
- 所以真正生成字节码时看到的 AST 可能已经被优化。

### 11. ASTToBytecodeVisitor：真正生成字节码

负责什么：

- 遍历 AST，根据合约语义生成脚本指令。

文件：

- `src/compiler/ast_to_bytecode_visitor.h`
- `src/compiler/ast_to_bytecode_visitor.cpp`

主要函数：

- `ASTToBytecodeVisitor::visit(ContractNode&)`
- `ASTToBytecodeVisitor::visit(FunctionNode&)`
- `ASTToBytecodeVisitor::visit(BlockNode&)`
- `ASTToBytecodeVisitor::visit(IfNode&)`
- `ASTToBytecodeVisitor::visit(ForNode&)`
- `ASTToBytecodeVisitor::visit(VarDeclNode&)`
- `ASTToBytecodeVisitor::visit(AssignNode&)`
- `ASTToBytecodeVisitor::visit(ReturnNode&)`
- `ASTToBytecodeVisitor::visit(LiteralNode&)`
- `ASTToBytecodeVisitor::visit(IdentifierNode&)`
- `ASTToBytecodeVisitor::visit(OpNode&)`
- `ASTToBytecodeVisitor::visit(CallNode&)`
- `ASTToBytecodeVisitor::visit(MethodCallNode&)`
- `ASTToBytecodeVisitor::processGenericFunctionCall`
- `ASTToBytecodeVisitor::privateFunctionResolution`
- `ASTToBytecodeVisitor::adjustStackToMatch`

输入：

- AST。
- `BytecodeGenerator`。
- 符号表 / 栈状态。

关键处理：

- 公开函数会生成 unlock 模板和对应脚本。
- 私有函数和库函数不会单独输出，而是在调用处内联。
- 表达式会根据当前栈位置生成 `OP_ROLL`、`OP_PICK`、`OP_SWAP` 等指令来调整栈。
- 内置函数通过 `OpFunctionFactory` 或 `BuiltinFunctionFactory` 转换成 opcode。
- 大写 `Return(...)` 会生成返回表达式和 `OP_RETURN`。
- 小写 `return` 会保留当前栈上的返回值，供调用者继续使用。
- `IfNode` 会生成 `OP_IF` / `OP_NOTIF` 和 `OP_ENDIF`，并校验两个分支的栈状态。
- `ForNode` 使用 `PreAnalysisVisitor` 提前计算好的静态迭代信息展开循环。

输出：

- 字节码指令列表。
- unlock 模板。
- 调试信息。

调用其他模块：

- `BytecodeGenerator`
- `Scope`
- `SymbolTable`
- `OpFunctionFactory`
- `BuiltinFunctionFactory`
- `BuiltinStruct`
- `ErrorManager`

理解重点：

- 这是最核心、最复杂的代码生成模块。
- 它不是简单地“看到表达式就发 opcode”，而是一直维护一套栈模型。

### 12. BytecodeGenerator：指令输出器

负责什么：

- 把 opcode、hex、占位符等规范化成指令列表。

文件：

- `src/bytecode/bytecode_generator.h`
- `src/bytecode/bytecode_generator.cpp`

主要函数：

- `BytecodeGenerator::emit(BytOpcode opcode)`
- `BytecodeGenerator::emit(BytOpcode opcode, const std::string& operand)`
- `BytecodeGenerator::emit(const std::string& hex)`
- `BytecodeGenerator::emitUnlock`
- `BytecodeGenerator::emitUnlockName`
- `BytecodeGenerator::instructions`

输入：

- opcode 枚举。
- 十六进制脚本片段。
- `<param>` 这类占位符。

关键处理：

- opcode 枚举转成 hex。
- 十六进制脚本按 pushdata 或 opcode 边界拆分。
- `<self.X>` 或 `<param>` 这类非 hex 占位符保留为独立指令。

输出：

- `std::vector<std::string>` 指令列表。

调用其他模块：

- `bytecode_opcodes.h`

理解重点：

- 这里生成的是中间形态的“指令字符串列表”，最后由 `ExportResultsPass` 拼成 hex 和 asm。

### 13. Scope / SymbolTable：编译期栈模型

负责什么：

- 维护主栈、altstack、fixed area 和符号绑定。
- 帮助代码生成器知道某个变量现在在栈的什么位置。

文件：

- `src/bytecode/scope.h`
- `src/bytecode/scope.cpp`
- `src/bytecode/symtab.h`
- `src/bytecode/symtab.cpp`

主要函数：

- `Scope::push`
- `Scope::pop`
- `Scope::roll`
- `Scope::pick`
- `Scope::dropAt`
- `Scope::defineSymbol`
- `Scope::defineArray`
- `Scope::defineCompoundType`
- `SymbolTable::getPos`
- `SymbolTable::compareStackState`

输入：

- 变量名。
- 类型。
- 数据来源。
- 栈操作请求。

关键处理：

- 主栈 top 是最近生成或使用的值。
- `getPos` 用于找变量相对栈顶的位置。
- `roll` / `pick` / `dropAt` 会改变栈状态并对应生成脚本操作。
- `compareStackState` 用于 if 分支后比较两个分支的栈是否一致。

输出：

- 更新后的栈状态。
- 给代码生成器的位置信息。

调用其他模块：

- `OpStack`
- `StackElement`

理解重点：

- 读 `ASTToBytecodeVisitor` 时如果看不懂为什么发出某个 `OP_ROLL`，通常要结合 `Scope::getPos`、`Scope::roll` 和当前符号表一起看。

### 14. 内置函数和内置对象

负责什么：

- 把合约里的高级函数名映射到底层脚本 opcode。

主要文件：

- `src/bytecode/bytecode_operation_functions.h`
- `src/bytecode/bytecode_builtin_function.h`
- `src/bytecode/bytecode_builtin_struct.h`

主要类 / 函数：

- `OpFunctionFactory::createFunction`
- `BuiltinFunctionFactory::createFunction`
- `BuiltinStruct::getMember`

输入：

- 函数名，例如 `Hash160`、`CheckSig`、`EqualVerify`、`SetAlt`、`Clone`。
- 参数列表。

关键处理：

- 普通 opcode 函数由 `OpFunctionFactory` 处理。
- 特殊语义函数由 `BuiltinFunctionFactory` 处理。
- `self` 和 `BVM` 成员访问由 `bytecode_builtin_struct.h` 处理。

输出：

- opcode hex。
- 栈状态变化。

调用其他模块：

- `ASTToBytecodeVisitor::processGenericFunctionCall`
- `PreAnalysisVisitor::visit(CallNode&)`

理解重点：

- 如果要新增一个内置函数，通常不能只改一个地方：
  - 可能要改文档 `doc/BUILTIN_FUNCTION_DOC.md`。
  - 可能要改 `OpFunctionFactory` 或 `BuiltinFunctionFactory`。
  - 可能要改 `PreAnalysisVisitor` 的消费规则。
  - 可能要补测试。

### 15. ExportResultsPass：最终输出

负责什么：

- 把编译结果写成 JSON。

文件：

- `src/export_results_pass.h`

主要函数：

- `ExportResultsPass::execute`

输入：

- `"bytcode"`
- `abi`
- `unlock`
- `constructorParams`
- `structs`
- `all_functions`
- `code_file_name`

关键处理：

- 跳过注释类指令。
- 拼接 lock hex。
- 转成 lock asm。
- 生成 metadata。
- 写文件。

输出：

- `<code_file_name>.json`

调用其他模块：

- `ConfigManager::generateMetadata`
- `tbc::script_decoder::hex_to_asm`
- nlohmann/json

理解重点：

- 如果编译成功但输出 JSON 内容不对，最后一定要看这里。

## 五、关键调用链

### 普通编译调用链

```text
用户命令
-> main.cpp::main
-> main.cpp::parseCommandLineArgs
-> Logger::initialize
-> ConfigManager::initialize
-> 读取 .ct 源文件
-> 创建 PassContext
-> PassManager::registerPass / enablePass
-> PassManager::run
-> LexerPass::execute
-> ImportResolver::resolveWithMap
-> Lexer::tokenize
-> ParserPass::execute
-> Parser::parseContract
-> ASTToBytecodePass::execute
-> ASTToBytecodeConverter::convert
-> CollectSymbolsVisitor::checkUniqueness
-> StaticInfoVisitor::visit
-> PreAnalysisVisitor::analyze
-> ConstantFolder::fold
-> ASTToBytecodeVisitor::visit
-> BytecodeGenerator::emit
-> BytecodePeepholePass::execute
-> BytecodeFinalizePass::execute
-> ExportResultsPass::execute
-> 写出 <contract>.json
```

### import 标准库调用链

```text
.ct 源码里的 import std.p2pkh
-> LexerPass::execute
-> ImportResolver::resolveWithMap
-> ImportResolver 查找 stdlib 根目录
-> 读取 stdlib/std/p2pkh.ct
-> 展开 Library std.p2pkh
-> Lexer::tokenize
-> Parser::parseLibrary
-> ASTToBytecodePass 把 library members 合并到 contract members
-> ASTToBytecodeVisitor 遇到库函数调用时内联处理
```

### 公开函数编译调用链

```text
ContractNode
-> ASTToBytecodeVisitor::visit(ContractNode&)
-> ASTToBytecodeVisitor::visit(FunctionNode&)
-> 处理公开函数参数
-> BytecodeGenerator::emitUnlock / emitUnlockName
-> ASTToBytecodeVisitor::visit(BlockNode&)
-> 逐条 visit 语句
-> visit(ReturnNode&) 生成 OP_RETURN
-> BytecodeFinalizePass 补齐 OP_INVALIDOPCODE
-> ExportResultsPass 写入 lock.hex / lock.asm / abi / unlock
```

### 私有函数或库函数调用链

```text
CallNode
-> ASTToBytecodeVisitor::visit(CallNode&)
-> ASTToBytecodeVisitor::processGenericFunctionCall
-> 查找 m_privateFunctions
-> ASTToBytecodeVisitor::processArgsToTop
-> ASTToBytecodeVisitor::privateFunctionResolution
-> 参数名绑定到实参
-> 内联执行私有函数 / 库函数 body
-> 小写 return 保留返回值在栈上
-> 调用点继续生成后续指令
```

### 调试模式调用链

```text
用户命令 --debug
-> main.cpp::main
-> main.cpp::startDebugger
-> DebuggerCore::compileSource
-> 同一套 Pass 流水线编译源码
-> 生成 bytecode 和 debug info
-> BVMSimulator
-> BreakpointManager
-> promptForFunctionParameters
-> CLIDebugger::run
-> 用户输入 step / run / break / stack 等命令
```

## 六、数据流

### 1. 数据从哪里进入项目？

主要入口：

- 命令行参数：进入 `main.cpp::parseCommandLineArgs`。
- `.ct` 源文件内容：进入 `main.cpp::main`，被读成 `std::string tbc_file`。
- `user_preferences.json`：进入 `ConfigManager::initialize`。
- 标准库文件：由 `ImportResolver::resolveWithMap` 根据 import 语句读取。

### 2. 进入后被哪个模块接收？

普通编译时，核心数据先被 `main.cpp` 接收，然后写入 `PassContext`：

```text
source_code
source_file_path
code_file_name
allow_subscope_altstack
enable_debug
debug_output_file
```

后续每个 Pass 从 `PassContext` 读取自己需要的数据，再写入新的中间结果。

### 3. 经过哪些转换、解析、校验或计算？

完整数据转换如下：

```text
源码字符串
-> ImportResolver 展开 import
-> Lexer 生成 Token
-> Parser 生成 AST
-> CollectSymbolsVisitor 做符号检查
-> StaticInfoVisitor 生成 ABI / unlock / structs
-> PreAnalysisVisitor 做变量所有权和栈安全分析
-> ConstantFolder 做 AST 优化
-> ASTToBytecodeVisitor 生成字节码指令
-> BytecodePeepholePass 优化指令序列
-> BytecodeFinalizePass 对齐填充
-> ExportResultsPass 生成 JSON
```

### 4. 中间数据结构是什么？

主要中间结构：

- `std::string source_code`
- `ExpandedSource`：import 展开后的源码和 SourceMap。
- `std::vector<Token>`：词法分析结果。
- `ContractNode`：AST 根节点。
- `json abi`
- `json unlock`
- `json constructorParams`
- `json structs`
- `json all_functions`
- `std::pair<std::vector<std::string>, std::unordered_map<std::string, std::string>>`：字节码和 unlock 映射。
- `Scope` / `SymbolTable` / `OpStack`：代码生成期间的编译期栈状态。

### 5. 最终数据输出到哪里？

最终输出：

- `<code_file_name>.json`：由 `ExportResultsPass::execute` 写入当前工作目录。

调试相关输出：

- 默认 `<code_file_name>.debug`。
- 或者用户通过 `--debug-output` 指定的文件。
- 调试模式下 `DebuggerCore::compileSource` 会创建临时 debug 文件并读回。

### 6. 如果有错误，错误是如何处理和传递的？

错误管理核心在：

- `src/error/error_manager.h`
- `src/error/error_manager.cpp`

常见错误宏：

- `LEXICAL_ERROR`
- `SYNTAX_ERROR`
- `SEMANTIC_ERROR`
- `TYPE_ERROR`
- `INTERNAL_ERROR`

传递方式：

1. Lexer / Parser / Visitor / Pass 发现问题。
2. 调用 `ErrorManager` 或错误宏。
3. `ErrorManager` 记录错误，并结合 SourceMap 输出文件、行号、列号、上下文。
4. `main.cpp` 在 Pass 运行后检查 `ErrorManager::hasErrors()`。
5. 如果有错误，返回 `1`。

有些严重错误会抛异常，`main.cpp` 用 `try/catch` 捕获：

```text
pm.run(pipelineData)
-> catch std::exception
-> ErrorManager::printAllErrors
-> return 1
```

## 七、结合一个具体例子讲

这里用一个小合约来走完整流程。示例文件来自：

- `test/debugger_regression/debug_line_mapping_basic.ct`

合约内容大意是：

```text
Contract DebugLineMappingBasic:
    def test_line_mapping(a: int, b: int, c: int):
        first: int = a + b
        second: int = first + c
        third: int = second + 1
        Return(third)
```

### 1. 输入是什么？

命令：

```bash
./build/bin/utxo_interpreter test/debugger_regression/debug_line_mapping_basic.ct
```

核心输入：

- 文件路径：`test/debugger_regression/debug_line_mapping_basic.ct`
- 源码字符串：整个合约文本。

### 2. 第一个接收它的文件 / 函数是谁？

第一个接收者：

- `main.cpp::main`

它会：

1. 调用 `parseCommandLineArgs` 解析文件路径。
2. 读取源码到 `tbc_file`。
3. 写入 `PassContext` 的 `source_code`。
4. 启动 Pass 流水线。

### 3. 它经过了哪些函数？

真实顺序可以简化为：

```text
main.cpp::main
-> LexerPass::execute
-> ImportResolver::resolveWithMap
-> Lexer::tokenize
-> ParserPass::execute
-> Parser::parseContract
-> Parser::parseFunction
-> Parser::parseStatement
-> Parser::parseExpression
-> ASTToBytecodePass::execute
-> ASTToBytecodeConverter::convert
-> CollectSymbolsVisitor::checkUniqueness
-> StaticInfoVisitor::visit
-> PreAnalysisVisitor::analyze
-> ConstantFolder::fold
-> ASTToBytecodeVisitor::visit(FunctionNode&)
-> ASTToBytecodeVisitor::visit(VarDeclNode&)
-> ASTToBytecodeVisitor::visit(OpNode&)
-> ASTToBytecodeVisitor::visit(ReturnNode&)
-> BytecodePeepholePass::execute
-> BytecodeFinalizePass::execute
-> ExportResultsPass::execute
```

### 4. 每一步数据发生了什么变化？

第 1 步，源码进入 `main.cpp`：

```text
"Contract DebugLineMappingBasic: ..."
```

变成 `PassContext` 里的：

```text
source_code = 源码字符串
source_file_path = test/debugger_regression/debug_line_mapping_basic.ct
code_file_name = debug_line_mapping_basic
```

第 2 步，`LexerPass::execute`：

源码被展开 import。这个例子没有 import，所以展开后基本不变。

然后 `Lexer::tokenize` 生成类似这样的 Token：

```text
TOKEN_CONTRACT
TOKEN_IDENTIFIER(DebugLineMappingBasic)
TOKEN_COLON
TOKEN_NEWLINE
TOKEN_INDENT
TOKEN_DEF
TOKEN_IDENTIFIER(test_line_mapping)
...
TOKEN_RETURN
...
TOKEN_EOF
```

第 3 步，`ParserPass::execute`：

Token 变成 AST：

```text
ContractNode(name = DebugLineMappingBasic)
  FunctionNode(name = test_line_mapping)
    params = a:int, b:int, c:int
    BlockNode
      VarDeclNode(first:int, expr = a + b)
      VarDeclNode(second:int, expr = first + c)
      VarDeclNode(third:int, expr = second + 1)
      ReturnNode(expr = third, isValueReturn = false)
```

第 4 步，`StaticInfoVisitor`：

因为 `test_line_mapping` 是公开函数，所以生成 ABI：

```json
[
  {
    "type": "function",
    "name": "test_line_mapping",
    "index": 0,
    "params": [
      {"name": "a", "type": "int"},
      {"name": "b", "type": "int"},
      {"name": "c", "type": "int"}
    ]
  }
]
```

并生成 unlock 模板：

```text
<a><b><c>
```

第 5 步，`PreAnalysisVisitor`：

它检查：

- `a`、`b`、`c` 是函数参数。
- `first` 来自 `a + b`。
- `second` 来自 `first + c`。
- `third` 来自 `second + 1`。
- `Return(third)` 消费或输出最终结果。

这个例子没有 `.Clone()`、`SetAlt`、复杂结构体或 Range 循环，所以静态分析比较直接。

第 6 步，`ConstantFolder`：

`second + 1` 中的 `1` 是常量，但 `second` 不是常量，所以不能把整条表达式折叠成字面量。

不过后续代码生成会把 `x + 1` 优化成 `OP_1ADD`。

第 7 步，`ASTToBytecodeVisitor`：

公开函数参数先进入编译期栈模型：

```text
a
b
c
```

处理：

```text
first = a + b
```

会根据当前栈位置移动 `a`、`b` 到合适位置，然后发出 `OP_ADD`。

处理：

```text
second = first + c
```

继续发出加法相关指令。

处理：

```text
third = second + 1
```

会利用 `x + 1` 优化，生成 `OP_1ADD`。

处理：

```text
Return(third)
```

会把 `third` 放到栈顶并发出 `OP_RETURN`。

第 8 步，优化和 finalize：

`BytecodePeepholePass` 做局部消除和合并。

`BytecodeFinalizePass` 在最后一个 `OP_RETURN` 后面填充 `OP_INVALIDOPCODE`，使脚本长度按 64 字节对齐。

### 5. 最后输出了什么？

实际编译这个例子时，输出 JSON 中核心字段类似：

```json
{
  "abi": [
    {
      "index": 0,
      "name": "test_line_mapping",
      "params": [
        {"name": "a", "type": "int"},
        {"name": "b", "type": "int"},
        {"name": "c", "type": "int"}
      ],
      "type": "function"
    }
  ],
  "unlock": {
    "test_line_mapping": "<a><b><c>"
  },
  "lock": {
    "asm": "OP_ROT OP_ROT OP_ADD OP_ADD OP_1ADD OP_RETURN ...",
    "hex": "7b7b93938b6a..."
  }
}
```

这里：

- `OP_ROT OP_ROT`：调整栈上参数位置。
- `OP_ADD OP_ADD`：执行两次加法。
- `OP_1ADD`：执行 `+ 1` 优化。
- `OP_RETURN`：大写 `Return(...)` 生成的脚本返回。
- 后续 `OP_INVALIDOPCODE`：finalize 阶段填充。

## 八、最适合新人先看的代码

### 第一批应该看的 3-5 个文件

#### 1. main.cpp

重点看：

- `parseCommandLineArgs`
- `main`
- `startDebugger`

带着问题看：

- 用户命令是如何变成编译器内部配置的？
- `PassContext` 里放了哪些 key？
- 普通编译和调试模式在哪里分叉？

#### 2. src/pass/pass_manager.cpp 和 src/pass/pass_context.h

重点看：

- `PassManager::run`
- `PassManager::resolveDependencies`
- `PassContext::set`
- `PassContext::get`

带着问题看：

- 编译阶段如何排序？
- 一个 Pass 的输出如何成为下一个 Pass 的输入？
- 为什么这个项目适合用流水线模型？

#### 3. src/lexer_pass.h 和 src/parser_pass.h

重点看：

- `LexerPass::execute`
- `ParserPass::execute`

带着问题看：

- 源码进入编译器后第一步发生了什么？
- import 是在什么时候展开的？
- Token 是在哪里变成 AST 的？

#### 4. src/compiler/ast_to_bytecode_converter.h

重点看：

- `ASTToBytecodeConverter::convert`

带着问题看：

- AST 到字节码前要经过哪些检查？
- 静态信息、所有权分析、常量折叠、代码生成的顺序是什么？

#### 5. src/compiler/ast_to_bytecode_visitor.cpp

重点看：

- `visit(FunctionNode&)`
- `visit(VarDeclNode&)`
- `visit(OpNode&)`
- `visit(CallNode&)`
- `processGenericFunctionCall`
- `visit(ReturnNode&)`

带着问题看：

- 一个函数参数如何进入栈模型？
- 一个表达式如何变成 opcode？
- 内置函数如何被映射到底层 opcode？
- 大写 `Return` 和小写 `return` 的差别是什么？

### 可以暂时跳过的文件

新人第一遍可以暂时跳过：

- `src/debugger/`：除非你当前目标是调试器。
- `scripts/`：除非你当前目标是发布或跨平台构建。
- `docker/`：除非你要搭建统一构建环境。
- `slides/`、`thesis/`、`project_doc/`：里面可能有背景和设计材料，但第一遍读运行流程可以先不看。
- 大量 `.ct` 示例：第一遍选 1-2 个典型例子即可，例如 `wallet.ct` 和 `test/debugger_regression/debug_line_mapping_basic.ct`。

## 九、总结项目运行逻辑

### 1. 这个项目像一个什么样的流程？

它像一条编译流水线。

输入端是一份 `.ct` 合约源码，中间经过多个工位：

```text
展开 import
-> 切词
-> 解析语法树
-> 检查符号和所有权
-> 收集 ABI
-> 生成脚本指令
-> 优化脚本
-> 对齐填充
-> 导出 JSON
```

最后输出端是一份可以被链上虚拟机或相关工具消费的合约 JSON。

### 2. 它最核心的 3 个模块是什么？

第一，Pass 流水线：

- `main.cpp`
- `src/pass/pass_manager.cpp`
- `src/pass/pass_context.h`

它决定项目怎么运行起来。

第二，语法前端：

- `src/lexer/`
- `src/parser/`
- `src/ast/`

它决定 `.ct` 源码如何变成 AST。

第三，编译后端：

- `src/compiler/`
- `src/bytecode/`

它决定 AST 如何变成 BVM / Bitcoin Script 字节码。

### 3. 它的运行顺序可以简化成哪几步？

可以简化成 7 步：

```text
1. main.cpp 读取命令行和源文件
2. LexerPass 展开 import 并生成 Token
3. ParserPass 把 Token 解析成 AST
4. ASTToBytecodePass 做静态检查、ABI 收集和字节码生成
5. BytecodePeepholePass 优化字节码
6. BytecodeFinalizePass 做 OP_RETURN 后的填充和对齐
7. ExportResultsPass 写出 JSON
```

### 4. 如果我要修改功能，应该从哪里入手？

如果要改命令行参数：

- 从 `main.cpp::parseCommandLineArgs` 入手。

如果要改语言语法：

- 从 `doc/GRAMMAR_SPECIFICATION.md` 和 `src/parser/parser.cpp` 入手。
- 如果新增 Token，还要改 `src/include/token_type.h` 和 `src/lexer/lexer.cpp`。

如果要改语义检查：

- 从 `src/compiler/pre_analysis_visitor.cpp` 入手。

如果要新增内置函数：

- 从 `src/bytecode/bytecode_operation_functions.h` 或 `src/bytecode/bytecode_builtin_function.h` 入手。
- 同时检查 `src/compiler/pre_analysis_visitor.cpp` 是否需要更新消费规则。
- 再检查 `src/compiler/ast_to_bytecode_visitor.cpp::processGenericFunctionCall`。

如果要改最终 JSON 输出：

- 从 `src/export_results_pass.h::ExportResultsPass::execute` 入手。
- metadata 则看 `src/config/config_manager.cpp::ConfigManager::generateMetadata`。

如果要改字节码生成逻辑：

- 从 `src/compiler/ast_to_bytecode_visitor.cpp` 入手。
- 同时看 `src/bytecode/scope.cpp`、`src/bytecode/symtab.cpp` 和 `src/bytecode/bytecode_generator.cpp`。

### 5. 如果我要排查 bug，应该从哪里开始？

先看 bug 出现在哪个阶段：

```text
启动参数错误
-> main.cpp::parseCommandLineArgs

找不到 import / 标准库
-> src/lexer/import_resolver.cpp::ImportResolver::resolveWithMap

词法错误
-> src/lexer/lexer.cpp::Lexer::scanToken

语法错误
-> src/parser/parser.cpp::Parser::parseContract / parseStatement / parseExpression

变量被消费、Clone、Move、SetAlt 相关错误
-> src/compiler/pre_analysis_visitor.cpp

ABI / unlock 不对
-> src/compiler/static_info_visitor.h::StaticInfoVisitor

生成 opcode 不对
-> src/compiler/ast_to_bytecode_visitor.cpp
-> src/bytecode/bytecode_operation_functions.h
-> src/bytecode/bytecode_builtin_function.h

输出 JSON 不对
-> src/export_results_pass.h::ExportResultsPass::execute

调试器行为不对
-> src/debugger/core/debugger_core.cpp::DebuggerCore::compileSource
-> src/debugger/interface/cli_debugger.h
-> src/debugger/vm/bvm_simulator.*
```

一个很实用的排查顺序是：

```text
1. 先用最小 .ct 文件复现问题
2. 看错误信息是 lexical / syntax / semantic / type 里的哪一种
3. 对照 Pass 顺序定位阶段
4. 如果是生成结果不对，打开输出 JSON 看 abi、unlock、lock.asm、lock.hex 哪个先异常
5. 再回到对应 Visitor 或 Pass
```

## 附：一个最小心智模型

读这个项目时可以一直记住这句话：

```text
main.cpp 不直接编译合约，它只是把源码放进 PassContext；
真正的编译由 PassManager 串起各个 Pass；
真正的业务核心在 ASTToBytecodeConverter 和 ASTToBytecodeVisitor；
最终结果由 ExportResultsPass 写成 JSON。
```

如果只看一条主线，请按这个顺序打开：

```text
main.cpp
-> src/pass/pass_manager.cpp
-> src/lexer_pass.h
-> src/parser_pass.h
-> src/ast_to_bytecode_pass.h
-> src/compiler/ast_to_bytecode_converter.h
-> src/compiler/pre_analysis_visitor.cpp
-> src/compiler/ast_to_bytecode_visitor.cpp
-> src/bytecode/bytecode_generator.cpp
-> src/export_results_pass.h
```
