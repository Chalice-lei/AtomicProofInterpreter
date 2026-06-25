# AtomicProofCompiler 解释器改造计划

生成日期: 2026-05-22

本文档基于当前项目源码分析，梳理现有编译器架构、核心模块、编译流程、AST/字节码/目标代码生成方式，并给出将项目改造为解释器的可执行实施路线。

## 1. 当前编译器工作流程梳理

当前项目是一个 C++20 编写的 AtomicProof/TBC 合约编译器。整体流程不是传统的 AST -> IR -> Bytecode -> Machine Code 多层架构，而是:

```text
.ct source
  -> ImportResolver
  -> Lexer
  -> Parser
  -> AST
  -> CollectSymbolsVisitor
  -> StaticInfoVisitor
  -> PreAnalysisVisitor
  -> ConstantFolder
  -> ASTToBytecodeVisitor
  -> BytecodeGenerator
  -> BytecodePeepholePass
  -> BytecodeFinalizePass
  -> ExportResultsPass
  -> <contract>.json
```

入口在 `main.cpp`。正常编译模式下，`main()` 读取 `.ct` 文件后创建 `PassContext`，注册并运行以下 Pass:

```text
LexerPass
ParserPass
ASTToBytecodePass
BytecodePeepholePass
BytecodeFinalizePass
ExportResultsPass
```

`PassManager::run()` 会根据每个 Pass 的 `getDependencies()` 做依赖排序后执行。`PassContext` 是强类型 key-value 容器，当前流水线主要传递 `source_code`、`source_file_path`、`tokens`、`ast`、`bytcode`、`abi`、`unlock`、`structs`、`all_functions` 等数据。

### 1.1 前端

相关文件:

```text
src/lexer/import_resolver.*
src/lexer/lexer.*
src/parser/parser.*
src/ast/ast.*
src/ast/ast_visitor.h
src/lexer_pass.h
src/parser_pass.h
```

`LexerPass` 先调用 `ImportResolver::resolve()` 内联 `import`。import 支持两种形式:

```python
import std.p2pkh
import "./relative/path"
```

内联后再调用 `Lexer::tokenize()`。词法层支持 Python 风格缩进，通过 `TOKEN_INDENT` 和 `TOKEN_DEDENT` 表示块结构。

`ParserPass` 调用 `Parser::parseContract()`，解析结果为 `std::shared_ptr<ContractNode>`。当前语法支持:

- `Library` 块
- `Contract` 块
- `Struct` 定义
- 普通函数
- `__init__` 构造函数
- 变量声明
- 数组声明
- 赋值
- 解构赋值
- `if/else`
- `for target in Range(...)`
- 大写 `Return`
- 小写 `return`
- 函数调用、方法调用、字段访问、索引访问、二元表达式

AST 由 `src/ast/ast.h` 定义。核心节点包括:

```text
ContractNode
LibraryNode
FunctionNode
ConstructorNode
StructDefNode
BlockNode
IfNode
ForNode
AssignNode
ExprStmtNode
ReturnNode
VarDeclNode
ArrayDeclNode
ArrayDefNode
LiteralNode
IdentifierNode
CallNode
MethodCallNode
OpNode
FieldAccessNode
IndexAccessNode
BraceExprNode
DestructureAssignNode
```

### 1.2 中端分析

相关文件:

```text
src/compiler/collect_symbols_visitor.*
src/compiler/static_info_visitor.*
src/compiler/pre_analysis_visitor.*
src/compiler/constant_folder.*
src/compiler/ast_to_bytecode_converter.h
```

`ASTToBytecodeConverter::convert()` 是当前中端和后端的总调度点。其顺序为:

1. `CollectSymbolsVisitor::checkUniqueness(ast)`
2. `StaticInfoVisitor::visit(ast)`
3. `PreAnalysisVisitor::analyze(ast)`
4. `ConstantFolder::fold(ast)`
5. `ASTToBytecodeVisitor::visit(ast)`

`CollectSymbolsVisitor` 负责函数名、结构体名、变量名与结构体名冲突等检查，并收集结构体定义。

`StaticInfoVisitor` 负责生成 ABI、构造函数参数、结构体 JSON、全部函数信息。它区分 public 函数和 private/library 函数。库函数 `fromLibrary == true` 时不会进入 ABI，但会进入 `all_functions`，供调试器使用。

`PreAnalysisVisitor` 是重要的语义检查模块。它维护变量来源和所有权状态:

```cpp
enum class DataSource {
    CONTRACT_MEMBER,
    CONSTANT_VALUE,
    STACK_DATA,
    BUILTIN_OBJECT
};

enum class VariableState {
    DECLARED,
    USED,
    CONSUMED
};
```

它会检查 move/consume 语义、数组元素所有权、字段所有权、`SetAlt/SetMain` 相关约束，以及 `Range(...)` for 循环。当前 for 循环只支持编译期可求值的 `Range(...)`，并会在 `ForNode` 中写入静态迭代列表。

`ConstantFolder` 做常量折叠、常量传播、死变量删除、死分支删除、空循环删除。它会直接改写 AST。

### 1.3 后端与字节码生成

相关文件:

```text
src/compiler/ast_to_bytecode_visitor.*
src/bytecode/bytecode_generator.*
src/bytecode/scope.*
src/bytecode/symtab.*
src/util/op_stack.*
src/bytecode/bytecode_opcodes.*
src/bytecode/bytecode_operation_functions.h
src/bytecode/bytecode_builtin_function.h
src/bytecode/bytecode_builtin_struct.h
```

当前没有显式 IR。`ASTToBytecodeVisitor` 同时承担:

- AST 遍历
- 语义相关补充处理
- 栈布局模拟
- 变量位置管理
- 内联私有函数
- 结构体和数组展开
- 内置函数映射
- 目标字节码生成
- 调试信息生成

`ASTToBytecodePass` 在调用 converter 前还负责把 `ContractNode::libraries` 合并到 `ContractNode::members` 前面，并把库函数标记为 `fromLibrary`。这段逻辑目前藏在 bytecode pass 中，对解释器改造不友好，应抽成共享步骤。

`ASTToBytecodeVisitor::visit(FunctionNode&)` 的关键行为:

- 函数名以下划线开头或 `fromLibrary == true` 时，登记到 `m_privateFunctions`，不立即发码。
- public 函数会清空 scope，展开参数，生成 unlock 模板。
- 结构体参数会扁平展开为字段。
- 数组参数会展开为元素或结构体字段。
- 函数体直接生成 Bitcoin/TBC Script 字节码。

`BytecodeGenerator` 维护两个指令缓冲:

```text
m_subInstruct
m_instruct
```

`emit()` 输入可以是 opcode hex、`0x...` 或占位符。对于纯 hex，它会按 pushdata 边界切成指令:

- `0x01` 到 `0x4b`: 直接 push
- `OP_PUSHDATA1/2/4`: 长度前缀加数据
- 其他字节: 单字节 opcode

`Scope`、`SymbolTable`、`OpStack` 是编译期栈布局模型，不是运行时解释器环境。它们追踪:

- 主栈位置
- 副栈位置
- fixed value
- 数组元数据
- 复合类型元数据
- 作用域新增符号
- keep 符号
- 形参到实参绑定
- 零成本重命名

### 1.4 目标产物

`ExportResultsPass` 输出 JSON，主要结构:

```json
{
  "metadata": {},
  "structs": [],
  "abi": [],
  "lock": {
    "asm": "...",
    "hex": "..."
  },
  "unlock": {},
  "constructorParams": [],
  "functions": []
}
```

`lock.hex` 是最终锁定脚本字节码。`unlock` 是各 public 函数的参数占位符模板。`functions` 包含 public 和 private 函数信息，调试器使用它来选择函数和输入参数。

### 1.5 现有字节码解释基础

相关文件:

```text
src/debugger/core/debugger_core.*
src/debugger/vm/bvm_simulator.*
src/debugger/vm/stack_state.*
src/debugger/interface/cli_debugger.*
src/debugger/info/debug_info.*
```

`DebuggerCore::compileSource()` 会复用完整编译流水线，生成 hex bytecode，然后调用 `hexToInstructions()` 把 `lock.hex` 解码成 VM 指令。

`BVMSimulator` 已经是一个目标字节码解释器，支持:

- 主栈和副栈
- 初始栈设置
- 执行范围设置
- PC
- run/pause/resume/stepIn/stepOver/stepOut
- 断点
- 调试事件回调
- 交易上下文
- `OP_PUSH_META`
- `OP_PARTIAL_HASH`
- 哈希、签名、算术、比较、逻辑、栈操作、控制流

因此，项目已经具备“字节码解释执行”的骨架，但它目前主要服务调试器，不是面向用户的解释器入口。

## 2. 模块复用、替换和删除建议

### 2.1 可直接复用

前端:

```text
src/lexer/import_resolver.*
src/lexer/lexer.*
src/parser/parser.*
src/ast/ast.*
src/ast/ast_visitor.h
src/lexer_pass.h
src/parser_pass.h
```

语义与元数据:

```text
src/compiler/collect_symbols_visitor.*
src/compiler/static_info_visitor.*
src/compiler/pre_analysis_visitor.*
src/compiler/constant_folder.*
src/error/error_manager.*
src/error/error_types.h
src/bytecode/type_validator.*
```

字节码解释:

```text
src/debugger/core/debugger_core.*
src/debugger/vm/bvm_simulator.*
src/debugger/vm/stack_state.*
src/bytecode/script_decoder.*
src/debugger/info/debug_info.*
```

内置函数元数据:

```text
src/bytecode/bytecode_operation_functions.h
src/bytecode/bytecode_builtin_function.h
src/bytecode/bytecode_builtin_struct.h
src/bytecode/bytecode_base_function.h
```

这些文件中已有函数名、参数数量、返回值数量、输入输出类型、opcode 映射等信息。AST 解释器可以复用这些元数据，但具体执行逻辑需要单独实现为 runtime builtin。

### 2.2 需要替换或旁路

AST 源语言解释模式下，应旁路以下模块:

```text
src/compiler/ast_to_bytecode_visitor.*
src/bytecode/bytecode_generator.*
src/bytecode_peephole_pass.h
src/bytecode_finalize_pass.h
src/export_results_pass.h
```

原因:

- `ASTToBytecodeVisitor` 的核心职责是发射 Bitcoin/TBC Script，而不是返回运行时值。
- `Scope/SymbolTable/OpStack` 表达的是编译期栈布局，不适合作为源语言运行时环境。
- peephole/finalize/export 都是编译产物后处理，不属于 AST 解释执行。

### 2.3 应抽出的共享模块

当前 `ASTToBytecodePass` 包含库合并逻辑。解释器也需要这一步，否则导入库函数不可见。建议新增:

```text
src/compiler/library_merger.h
src/compiler/library_merger.cpp
```

接口示例:

```cpp
class LibraryMerger
{
public:
    static size_t mergeIntoContract(ContractNode& contract);
};
```

然后让 `ASTToBytecodePass` 和未来的 `ASTInterpretPass` 共用它。

### 2.4 不建议删除

短中期不建议删除现有编译器后端。解释器应作为并行能力加入:

```text
compile mode: source -> json
bytecode run mode: source/json -> VM result
ast interpret mode: source -> runtime result
```

保留现有后端有两个价值:

- 作为 AST 解释器的行为基准。
- 继续支持当前链上脚本生成工作流。

## 3. 解释器目标架构设计

建议新增目录:

```text
src/interpreter/
  runtime_value.h
  runtime_value.cpp
  runtime_error.h
  runtime_error.cpp
  environment.h
  environment.cpp
  runtime_context.h
  runtime_context.cpp
  builtin_registry.h
  builtin_registry.cpp
  ast_interpreter.h
  ast_interpreter.cpp
  interpreter_pass.h
  bytecode_runner.h
  bytecode_runner.cpp
```

### 3.1 RuntimeValue

解释器需要一个真正的运行时值系统。建议定义:

```cpp
enum class RuntimeType {
    Void,
    Int,
    Bool,
    Bytes,
    String,
    Address,
    Array,
    Struct,
    BuiltinObject
};

class RuntimeValue
{
public:
    RuntimeType type() const;
    std::string declaredType() const;
    std::vector<uint8_t> toScriptBytes() const;
    int64_t toInt() const;
    bool truthy() const;
};
```

底层建议保留 bytes 表示，同时提供 int/bool/string 的解释视图。原因是项目语言强绑定 Bitcoin/TBC Script，很多 builtin 的真实语义是字节数组操作。

### 3.2 RuntimeSlot

变量不仅有值，还需要状态:

```cpp
enum class OwnershipState {
    Available,
    Consumed,
    Deleted
};

enum class StorageClass {
    MainStack,
    AltStack,
    Fixed,
    SelfMember,
    BuiltinObject
};

struct RuntimeSlot
{
    RuntimeValue value;
    std::string declaredType;
    OwnershipState ownership;
    StorageClass storage;
};
```

这可以承接现有语言中的 move/consume 语义、`SetAlt`、`SetMain`、`Delete`、`Keep`、`Clone`。

### 3.3 Environment

运行时作用域不同于编译期 `Scope`。建议使用词法作用域链:

```cpp
class Environment
{
public:
    void define(const std::string& name, RuntimeSlot slot);
    RuntimeSlot& assign(const std::string& name, RuntimeValue value);
    RuntimeSlot& resolve(const std::string& name);
    std::shared_ptr<Environment> parent() const;
};
```

每次函数调用创建一个 function frame。普通块创建 block environment。for 循环可以复用同一个 block environment，以匹配当前编译器中“循环体不开新作用域，迭代间共享栈状态”的行为。

### 3.4 RuntimeContext

`RuntimeContext` 保存解释全过程共享信息:

```cpp
class RuntimeContext
{
public:
    ContractNode* contract;
    std::unordered_map<std::string, FunctionNode*> functions;
    std::unordered_map<std::string, StructDefNode*> structs;
    RuntimeValue self;
    RuntimeValue bvm;
    std::vector<CallFrame> callStack;
    TransactionContext tx;
};
```

需要在 AST 解释器启动前收集:

- public 函数
- private 函数
- library 函数
- struct 定义
- constructor 参数
- ABI 参数

### 3.5 BuiltinRegistry

解释器需要 runtime builtin，而不是只生成 opcode。建议分为几类:

纯函数:

```text
Push
Cat
Split
Size
NumToBin
BinToNum
Sha1
Sha256
Hash160
Hash256
Rmd160
PartialHash
```

验证函数:

```text
EqualVerify
NumEqualVerify
CheckSig
CheckSigVerify
MultiSig
MultiSigVerify
```

栈/所有权函数:

```text
Clone
Delete
Keep
SetAlt
SetMain
```

对象访问:

```text
self.<field>
BVM.version
BVM.locktime
BVM.inputCount
BVM.outputCount
BVM.inputsHash
BVM.unlockingInput
BVM.outputsHash
```

### 3.6 ASTInterpreter

`ASTInterpreter` 建议继承 `ASTVisitor`，但表达式求值需要返回值。由于当前 `ASTVisitor::visit()` 返回 `void`，可以采用内部栈或显式 `evalExpr()` 方法:

```cpp
class ASTInterpreter : public ASTVisitor
{
public:
    InterpretResult run(ContractNode& contract,
                        const std::string& functionName,
                        const std::vector<RuntimeValue>& args);

private:
    RuntimeValue evalExpr(ExprNode& expr);
    ControlFlow execStmt(StmtNode& stmt);
    ControlFlow execBlock(BlockNode& block, Environment& env);
};
```

语句执行返回控制流:

```cpp
enum class FlowKind {
    Normal,
    Return,
    Abort
};

struct ControlFlow {
    FlowKind kind;
    std::vector<RuntimeValue> values;
};
```

### 3.7 BytecodeRunner

短期先产品化现有 `BVMSimulator`:

```cpp
class BytecodeRunner
{
public:
    BytecodeRunResult runSource(const std::string& sourceFile,
                                const std::string& sourceCode,
                                const RunOptions& options);
};
```

它复用 `DebuggerCore::compileSource()`，但提供非交互式 CLI，而不是进入 `CLIDebugger`。

## 4. AST 解释、字节码解释、IR 解释三种方案对比

| 方案 | 实现成本 | 与最终脚本一致性 | 源级调试体验 | 适合阶段 | 主要风险 |
| --- | --- | --- | --- | --- | --- |
| 字节码解释执行 | 低 | 高 | 中 | 第一阶段 | 解释的是目标 Script，不是源语言 |
| AST 解释执行 | 中到高 | 中 | 高 | 第二阶段 | 需要重新实现 runtime 语义 |
| IR 解释执行 | 高 | 高 | 高 | 长期重构 | 当前没有显式 IR，改造面大 |

### 4.1 AST 解释执行

优点:

- 直接解释 `.ct` 源语言。
- 错误位置清晰，可直接绑定 AST `pos`。
- 适合 REPL、快速测试、教育和开发期验证。
- 可更自然地表达结构体、数组、函数调用和作用域。

缺点:

- 需要新建值系统、环境、调用栈、内置函数、所有权模型。
- 容易和最终 Script 行为产生偏差。
- `SetAlt/SetMain/Delete/Keep/Clone` 这类栈语义必须认真模拟，否则大量合约行为不一致。

适合目标:

- 真正的源语言解释器。
- 快速运行 `.ct` 函数并返回源级值。

### 4.2 字节码解释执行

优点:

- 当前已有 `BVMSimulator`。
- 最贴近最终 `lock.hex` 行为。
- 可以快速提供 `apc --run xxx.ct`。
- 可作为 AST 解释器的 golden reference。

缺点:

- 参数输入和变量查看依赖 ABI、unlock 模板、debug info。
- 错误多体现为栈错误或 opcode 错误，源语言语义不直观。
- 如果目标字节码生成有 bug，解释结果也会继承该 bug。

适合目标:

- 执行编译产物。
- 合约验证。
- 调试器增强。
- 差分测试基准。

### 4.3 IR 解释执行

优点:

- 可以统一编译器和解释器语义。
- 后续可支持多后端。
- 优化和分析更清晰。
- 比 AST 更接近执行语义，比 bytecode 更保留源级结构。

缺点:

- 当前项目没有显式 IR。
- 需要新增 AST lowering、IR 数据结构、IR interpreter、IR-to-bytecode backend。
- 需要逐步替换 `ASTToBytecodeVisitor` 中大量隐含的栈布局逻辑。

适合目标:

- 中长期架构升级。
- 降低后端复杂度。
- 支持更多目标代码或执行后端。

## 5. 推荐改造路线及理由

推荐路线:

```text
第一步: 产品化字节码解释器
第二步: 新增 AST 解释器
第三步: 用差分测试收敛语义
第四步: 条件成熟后抽出显式 IR
```

理由:

1. 现有 `BVMSimulator` 已经覆盖大量目标 opcode，是最快可落地的解释器基础。
2. 当前语言强绑定 Bitcoin/TBC Script 栈语义，字节码解释器可作为最可信运行基准。
3. 直接重写 AST 解释器风险较高，特别是结构体展开、数组字段、altstack、所有权、private function inline 等语义。
4. 显式 IR 是长期正确方向，但当前 `ASTToBytecodeVisitor` 体量大、职责混杂，不适合一次性替换。
5. 先做 bytecode runner 可以尽快形成用户可见能力，也能为 AST interpreter 建立回归测试体系。

## 6. 需要新增的核心模块

### 6.1 运行时环境

文件:

```text
src/interpreter/runtime_context.h
src/interpreter/runtime_context.cpp
src/interpreter/environment.h
src/interpreter/environment.cpp
```

职责:

- 管理函数表。
- 管理结构体定义。
- 管理调用栈。
- 管理当前环境。
- 管理 `self` 和 `BVM`。
- 管理交易上下文。

### 6.2 作用域

`Environment` 应支持:

- define
- assign
- resolve
- parent lookup
- block scope
- function scope
- shadowing 检查
- consumed/deleted 检查

注意: 不建议直接使用 `tbc::Scope`。`tbc::Scope` 是编译期栈位置模型，AST 解释器需要的是运行时变量绑定模型。

### 6.3 值系统

文件:

```text
src/interpreter/runtime_value.h
src/interpreter/runtime_value.cpp
```

需要支持:

- int
- bool
- string
- hex/bytes
- address
- array
- struct
- void
- builtin object

建议提供:

```cpp
RuntimeValue RuntimeValue::fromScriptNum(int64_t value);
RuntimeValue RuntimeValue::fromBytes(std::vector<uint8_t> bytes);
RuntimeValue RuntimeValue::fromString(std::string value);
std::vector<uint8_t> RuntimeValue::toScriptBytes() const;
int64_t RuntimeValue::toScriptNum() const;
bool RuntimeValue::truthy() const;
```

### 6.4 内存和所有权模型

需要新增:

```text
src/interpreter/runtime_slot.h
```

或放入 `environment.h`。

每个变量 slot 至少包含:

```text
RuntimeValue value
declaredType
ownership state
storage class
```

需要支持:

- `Clone`: 复制值，原值仍可用。
- `Delete`: 标记 deleted，并从环境或模拟栈位置移除。
- `Keep`: 防止作用域退出时清理。
- `SetAlt`: storage 从 `MainStack` 变为 `AltStack`。
- `SetMain`: storage 从 `AltStack` 变为 `MainStack`。

### 6.5 错误处理

文件:

```text
src/interpreter/runtime_error.h
src/interpreter/runtime_error.cpp
```

应接入 `ErrorManager`:

- runtime type mismatch
- use after consume
- use after delete
- undefined variable
- invalid field access
- invalid array index
- builtin arg count mismatch
- verify failed
- division by zero
- unsupported operation

每个错误尽量携带 AST `pos`，转换为 `SourceLocation`。

### 6.6 标准库和内置函数调用

文件:

```text
src/interpreter/builtin_registry.h
src/interpreter/builtin_registry.cpp
```

第一批建议实现:

```text
Push
Clone
Delete
Keep
SetAlt
SetMain
Cat
Split
Size
NumToBin
BinToNum
Equal
EqualVerify
NumEqual
NumEqualVerify
Sha1
Sha256
Hash160
Hash256
Rmd160
PartialHash
CheckSig
CheckSigVerify
```

其中签名验证可先做 mock 或显式返回配置值，再接入真实验证。

### 6.7 AST 解释器

文件:

```text
src/interpreter/ast_interpreter.h
src/interpreter/ast_interpreter.cpp
src/interpreter/interpreter_pass.h
```

`ASTInterpreter` 应支持:

- public 函数入口选择
- private/library 函数调用
- 构造函数初始化
- 变量声明和赋值
- return/Return
- if/else
- Range for
- field/index access
- array/struct literal
- destructure assign
- method call
- builtin call

### 6.8 字节码 runner

文件:

```text
src/interpreter/bytecode_runner.h
src/interpreter/bytecode_runner.cpp
```

短期包装:

```text
DebuggerCore::compileSource()
DebuggerCore::hexToInstructions()
BVMSimulator::setInitialStacks()
BVMSimulator::setTransactionData()
BVMSimulator::run()
```

## 7. 分阶段实施计划

### 阶段 0: 抽共享前端和库合并

目标:

- 让编译器和解释器都能拿到同样的已合并 AST。

任务:

1. 新增 `LibraryMerger`。
2. 从 `ASTToBytecodePass` 移出 library merge 逻辑。
3. 新增前端辅助函数，例如 `compileFrontendToAst()`。
4. 保持现有编译输出不变。

验收:

- 所有现有 `.ct` 编译 JSON 不变。
- import 库函数仍能被 public 函数调用。

### 阶段 1: 产品化字节码解释器

目标:

- 提供非交互式运行入口。

任务:

1. 新增 `BytecodeRunner`。
2. `main.cpp` 增加 CLI 参数:

```text
--run
--run-bytecode
--function <name>
--arg <value>
--txfile <path>
```

3. 支持从 ABI/functions 中选择 public 函数。
4. 支持参数解析，复用 `main.cpp` 当前 debugger 的参数解析逻辑，但迁移到可复用模块。
5. 输出最终 VM 状态、主栈、副栈、错误信息。

验收:

- `apc contract.ct --run-bytecode --function main --arg ...` 可执行。
- 执行结果与调试器模式一致。

### 阶段 2: Runtime 基础设施

目标:

- 为 AST 解释器提供运行时基础。

任务:

1. 新增 `RuntimeValue`。
2. 新增 `RuntimeSlot`。
3. 新增 `Environment`。
4. 新增 `RuntimeContext`。
5. 新增 `RuntimeError`。
6. 写单元测试覆盖值转换和作用域解析。

验收:

- 可构造 int/bool/string/hex/array/struct。
- 可完成变量 define/assign/resolve。
- use-after-delete/use-after-consume 可报错。

### 阶段 3: AST 解释器最小闭环

目标:

- 执行不含复杂内置函数的基础语言子集。

任务:

1. 实现 `LiteralNode`。
2. 实现 `IdentifierNode`。
3. 实现 `OpNode` 的 `+ - * / == != < > <= >= && ||`。
4. 实现 `VarDeclNode`。
5. 实现 `AssignNode`。
6. 实现 `BlockNode`。
7. 实现 `IfNode`。
8. 实现 `ForNode` 的静态 `Range(...)`。
9. 实现 `ReturnNode`。
10. 实现 public/private 函数调用。

验收:

- 基础变量、运算、if/else、Range for、private return 可运行。
- 错误行列能指向 AST 节点位置。

### 阶段 4: 内置函数和对象

目标:

- 支持现有合约常用内置函数。

任务:

1. 实现 `BuiltinRegistry`。
2. 复用 `bytecode_operation_functions.h` 中的函数名、参数数量、类型元数据。
3. 实现数据函数、算术函数、比较函数、哈希函数。
4. 实现 `self` 成员读取。
5. 实现 `BVM` 元数据读取。
6. 实现 `SetAlt/SetMain/Delete/Keep/Clone` 的解释语义。

验收:

- `test/test_builtin_function_and_object` 中基础用例可解释。
- `p2pkh` 类简单合约可解释。

### 阶段 5: 结构体、数组、复合类型

目标:

- 支持项目中复杂合约依赖的数据结构。

任务:

1. 实现 `StructDefNode` 注册。
2. 实现结构体实例化和字段访问。
3. 实现数组声明、数组字面量、索引访问。
4. 实现结构体数组字段访问。
5. 实现 compound type `__compound__`。
6. 实现解构赋值。

验收:

- `test/test_array_and_struct` 可解释。
- `test/test_struct_array_field` 可解释。
- `test/test_compound_type` 可解释。

### 阶段 6: AST 与 bytecode 差分测试

目标:

- 收敛 AST 解释器和目标字节码 VM 行为。

任务:

1. 新增测试驱动。
2. 对同一个 `.ct`、同一组输入，分别运行 AST interpreter 和 bytecode runner。
3. 比较:

```text
success/error
return value
main stack top
alt stack
verify failure
runtime error
```

4. 对差异分类:

```text
AST interpreter bug
bytecode generation bug
VM simulator bug
语言规范未定义
```

验收:

- 基础语句、函数、结构体、数组、常用 builtin 的差分通过。

### 阶段 7: 可选 IR 改造

目标:

- 把 `ASTToBytecodeVisitor` 中隐含的执行语义抽成显式 IR。

任务:

1. 新增 `src/ir/`。
2. 定义:

```text
IRModule
IRFunction
IRBasicBlock
IRInstruction
IRValue
IRType
```

3. 新增 `LowerASTToIR`。
4. 新增 `IRInterpreter`。
5. 新增 `IRToBytecode`。
6. 逐步让原 `ASTToBytecodeVisitor` 退化为 IR 后端或被替换。

验收:

- bytecode 输出保持一致。
- IR interpreter 和 bytecode VM 差分通过。

## 8. 每个阶段的代码改动范围

| 阶段 | 主要新增 | 主要修改 | 不应修改 |
| --- | --- | --- | --- |
| 阶段 0 | `src/compiler/library_merger.*` | `src/ast_to_bytecode_pass.h`, `CMakeLists.txt` | `ASTToBytecodeVisitor` 主逻辑 |
| 阶段 1 | `src/interpreter/bytecode_runner.*` | `main.cpp`, 可选 `DebuggerCore` 参数解析抽取 | `BVMSimulator` opcode 语义 |
| 阶段 2 | `runtime_value`, `environment`, `runtime_context`, `runtime_error` | `CMakeLists.txt` | 编译器后端 |
| 阶段 3 | `ast_interpreter.*`, `interpreter_pass.h` | `main.cpp`, pass 注册 | `BytecodeGenerator` |
| 阶段 4 | `builtin_registry.*` | 可能抽取部分 helper | 现有 builtin opcode 映射 |
| 阶段 5 | runtime struct/array 支持文件 | `ast_interpreter.*` | parser 语法 |
| 阶段 6 | test runner/scripts | 测试配置 | 生产逻辑 |
| 阶段 7 | `src/ir/*` | `ASTToBytecodeConverter`, 后端接入 | 旧后端可先保留 |

## 9. 可能遇到的技术风险和解决方案

### 9.1 AST 解释语义和 Script 语义不一致

风险:

源语言变量看起来像普通变量，但当前编译器实际使用主栈、副栈、ROLL、DROP、NIP、fixed stack 模拟变量生命周期。AST 解释器如果只做普通变量环境，会和目标脚本行为不一致。

方案:

- RuntimeSlot 中保留 `storage` 和 `ownership`。
- `SetAlt/SetMain/Delete/Keep/Clone` 必须有真实语义。
- 用 bytecode runner 做差分测试。

### 9.2 library merge 逻辑被后端私有化

风险:

AST interpreter 如果直接拿 ParserPass 的 AST，import 的库函数仍在 `ContractNode::libraries`，不会出现在函数表中。

方案:

- 阶段 0 先抽出 `LibraryMerger`。
- 编译器和解释器都使用同一合并逻辑。

### 9.3 private function 当前是内联语义

风险:

`ASTToBytecodeVisitor` 中 private/library 函数不是普通 call/return，而是调用时内联展开，并通过小写 `return` 标记栈上保留值。AST 解释器如果实现普通函数调用，某些栈清理行为可能不同。

方案:

- AST 解释器第一版可以实现普通调用，但 `return` 语义必须和当前规则一致。
- 对 private function 的多返回值 `{a, b}` 做专门测试。
- 差分测试覆盖 private function。

### 9.4 结构体和数组扁平化复杂

风险:

当前后端会把结构体、结构体数组、compound type 扁平化到栈元素。AST 解释器如果使用嵌套对象表示，会和 bytecode 栈布局不同。

方案:

- 源级 AST interpreter 内部可用嵌套对象。
- 但必须提供 `toFlattenedScriptValues()`，用于和 bytecode VM 对齐。
- 对字段顺序严格遵循 `StructDefNode::fields`。

### 9.5 BVM 和交易元数据

风险:

`BVM.unlockingInput`、`BVM.outputsHash` 等依赖交易上下文。没有上下文时，解释结果不可靠。

方案:

- 新增 `TransactionContext`。
- 没有显式传入时使用和 `BVMSimulator::TransactionData` 一致的默认值。
- CLI 支持 `--txfile`。

### 9.6 加密和签名验证

风险:

哈希函数容易实现，签名验证需要真实交易上下文和签名算法细节。

方案:

- 第一版把 `CheckSig` 设为可配置 mock。
- 第二版接入现有 VM 或独立 crypto provider。
- `CheckSigVerify` 在 mock false 时抛 runtime verify error。

### 9.7 当前 parser/visitor 返回值接口不适合解释器

风险:

`ASTVisitor::visit()` 返回 `void`，表达式解释需要返回 `RuntimeValue`。

方案:

- 不强行通过 visitor 返回值。
- 在 `ASTInterpreter` 中实现 `RuntimeValue evalExpr(ExprNode&)` 和 `ControlFlow execStmt(StmtNode&)`，内部用 `dynamic_cast` 分发。
- 或保留 visitor 只作外壳，表达式结果放成员变量栈中。

### 9.8 CLI 复杂度上升

风险:

编译模式、调试模式、字节码运行模式、AST 解释模式混在 `main.cpp` 中，会进一步膨胀入口文件。

方案:

- 新增 `src/cli/command_line.*` 或至少抽出 `CommandLineArgs`。
- 将 debugger 参数输入迁移到可复用参数解析模块。

## 10. 验证方式和测试计划

### 10.1 编译器回归

目标:

- 新增解释器不破坏现有编译器。

测试:

```bash
cmake --build build
./build/bin/utxo_interpreter test/contract_file/counter.ct
./build/bin/utxo_interpreter test/beginner's_tutorial_on_compiler/p2pkh.ct
```

检查:

- JSON 文件仍生成。
- `lock.hex` 非空。
- `abi`、`unlock`、`functions` 正常。
- 原有 debugger regression 不退化。

### 10.2 RuntimeValue 单元测试

覆盖:

- int 到 ScriptNum bytes。
- ScriptNum bytes 到 int。
- bool truthy。
- string bytes。
- hex bytes。
- array/struct 构造和访问。

### 10.3 Environment 单元测试

覆盖:

- define/resolve/assign。
- parent lookup。
- shadowing。
- undefined variable。
- consumed variable。
- deleted variable。
- alt/main storage 切换。

### 10.4 Builtin 单元测试

覆盖:

```text
Push
Clone
Delete
Keep
SetAlt
SetMain
Cat
Split
Size
NumToBin
BinToNum
EqualVerify
Sha256
Hash160
Hash256
PartialHash
```

### 10.5 AST interpreter 用例

优先测试目录:

```text
test/test_basic_statement/
test/test_function/
test/test_array_and_struct/
test/test_struct_array_field/
test/test_compound_type/
test/test_loop/
test/test_builtin_function_and_object/
```

每个测试记录:

```text
input source
entry function
args
expected success/error
expected return value
expected stack state if applicable
```

### 10.6 Bytecode runner 测试

覆盖:

- 从 `.ct` 编译后运行。
- 从已有 `.json` 的 `lock.hex` 运行。
- 指定 public 函数范围运行。
- 参数入栈顺序。
- 结构体参数扁平化顺序。
- `BVM` 交易元数据。

### 10.7 AST 与 bytecode 差分测试

测试流程:

```text
source + args
  -> ASTInterpreter
  -> AST result

source + args
  -> compiler
  -> BVMSimulator
  -> bytecode result

compare(AST result, bytecode result)
```

比较字段:

```text
success/error
error category
return value
main stack top
main stack size
alt stack size
verify failure
```

### 10.8 验收标准

第一阶段验收:

- `--run-bytecode` 可执行简单合约。
- 输出 VM 最终状态和栈内容。
- 不影响现有编译输出。

第二阶段验收:

- AST interpreter 可执行基础语句、函数、if/else、Range for。
- 错误带源码位置。

第三阶段验收:

- AST interpreter 可执行常用 builtin。
- p2pkh 简单合约 AST 解释和 bytecode VM 差分一致。

第四阶段验收:

- 结构体、数组、compound type 用例通过。
- 主要测试目录差分通过。

## 总结

当前项目已经具备很强的前端、静态分析和目标字节码模拟基础。真正缺少的是“源语言运行时模型”。因此不建议直接把现有 `ASTToBytecodeVisitor` 改写成解释器，而应以并行架构演进:

```text
保留现有编译器
产品化现有 BVMSimulator
新增 AST runtime interpreter
用差分测试对齐两者
长期抽出 IR
```

这样能最快获得可运行能力，也能最大限度降低对现有编译器产物的破坏风险。
