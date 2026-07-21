# 模板库系统 (Library Template System)

本文档描述 AtomicProof Compiler 的**模板库**机制——如何将可复用的函数与结构体封装为 `Library` 块，供任意用户合约通过 `import` 导入并直接调用。

---

## 1. 设计动机

### 1.1 问题

在引入 Library 之前，stdlib 模板（如 `stdlib/std/p2pkh.ct`）是一个完整的 `Contract`，通过 `import` 做源码级内联。这导致两个核心限制：

1. **单合约冲突**：解析器强制"一个翻译单元恰好一个 `Contract`"。import 展开后若用户文件也有 `Contract`，出现两段顶层合约会直接报语法错误。
2. **不可组合**：即使不冲突，模板的 `main` 直接成为最终入口，用户无法在其之上叠加额外的业务逻辑——即"整合约级复用"，无法做到函数级复用。

### 1.2 目标

参考 sCrypt 的 `import { P2PKH } from 'scrypt-ts'` 模型，让用户能：

```python
import std.p2pkh                          # 导入库

Contract MyWallet:                         # 定义自己的合约
    def main(signature: hex, pubKey: hex):
        ok = verifyP2PKH(signature, pubKey)                # 调用库函数
```

即 **import 模板后，在合约文件中直接使用该模板的函数，与自身代码自由组合**。

### 1.3 选型

| 方案 | 描述 | 采用 |
|---|---|---|
| 库 / 自由函数模型 | 模板为 free `def` + `Struct` 集合，无合约状态 | **是** |
| 继承 / extend 模型 | `Contract X extends Y`，复用父合约 main/成员 | 否 |
| 命名空间内联模型 | `std.p2pkh.verify(...)` 限定调用 | 否 |

选择库模型原因：改动最收敛（不需继承/命名空间），贴近 sCrypt 常见用法，与已有单合约体系正交。

---

## 2. 语法

### 2.1 Library 块定义

```bnf
library ::= "Library" library_name ":" NEWLINE INDENT [lib_member]+ DEDENT

library_name ::= IDENTIFIER ["." IDENTIFIER]*

lib_member ::= function | struct
```

- 关键字 `Library`（大写开头，与 `Contract`、`Struct` 风格一致）。
- 库名允许 dotted path（如 `std.p2pkh`），与 `import` 语法保持一致。
- 块内只允许 `def` 和 `Struct`，**禁止**：
  - 构造函数 `def __init__(...)`
  - 入口函数 `def main(...)`
- **允许**对 `self.<field>` 的引用：由于库成员在合并步之后与用户合约成员位于同一作用域，库函数内的 `self.<field>` 会解析为**导入它的合约**的成员变量。这构成一种"**约定式契约**"——使用此类库的合约必须声明相应成员，否则编译期符号解析失败。

### 2.2 顶层程序结构

```bnf
program ::= [library]* contract
```

一个翻译单元（经 import 展开后）由**零或多个 `Library` 块**加上**恰好一个 `Contract` 块**组成。`Library` 必须出现在 `Contract` 之前。

### 2.3 import 语法（不变）

```bnf
import_stmt ::= "import" dotted_path
             |  "import" QUOTED_PATH
```

import 行为与此前完全一致——由 `ImportResolver` 在词法分析之前做源码级内联展开：

- `import std.p2pkh` → 读取 `stdlib/std/p2pkh.ct` 并将其内容**就地替换** import 行。
- 去重（同一文件重复 import 为空）、循环检测照常生效。

展开后的源码中，库文件的 `Library std.p2pkh:` 块自然出现在用户 `Contract` 之前，由 parser 依次解析。

### 2.4 示例

**库文件** `stdlib/std/p2pkh.ct`：
```python
Library std.p2pkh:

    def verifyP2PKH(signature: hex, pubKey: hex):
        EqualVerify(Hash160(pubKey.Clone()), self.pubKeyHash)
        result = CheckSig(signature, pubKey)
        return result
```

**用户合约** `test/wallet.ct`：
```python
import std.p2pkh

Contract MyWallet:

    def main(signature: hex, pubKey: hex):
        ok = verifyP2PKH(signature, pubKey)
```

> 注：库函数内部的 `self.pubKeyHash` 会解析为 `MyWallet` 合约的成员变量。使用 `std.p2pkh` 的合约必须声明 `self.pubKeyHash`（由部署方在替换占位符时绑定 20 字节 Hash160）。

---

## 3. 编译器实现

### 3.1 流水线概览

```
┌──────────────┐    ┌──────────────┐    ┌──────────────────┐    ┌──────────────┐
│  ImportResolver  │ → │  Lexer/Parser  │ → │ Library Merge Step │ → │ AST Visitors   │
│  (源码内联)       │    │ (Library 识别)  │    │ (members 合并)     │    │ (符号/字节码)    │
└──────────────┘    └──────────────┘    └──────────────────┘    └──────────────┘
```

### 3.2 各阶段详解

#### 3.2.1 词法 (Lexer)

| 变更 | 文件 | 说明 |
|---|---|---|
| 新增 `TOKEN_LIBRARY` | `src/include/token_type.h` | 枚举 + 字符串映射 |
| 注册关键字 `Library` | `src/lexer/lexer.cpp` | keywords 表新增条目 |

#### 3.2.2 AST

| 变更 | 文件 | 说明 |
|---|---|---|
| `LibraryNode` 类 | `src/ast/ast.h` | 继承 `ASTNode`；字段：`name`（支持 dotted）、`members`（`vector<unique_ptr<ASTNode>>`）。`accept()` 为空实现——合并步之后 visitor 流水线不会遇到它 |
| `ContractNode::libraries` | `src/ast/ast.h` | 临时存储字段 `vector<unique_ptr<LibraryNode>>`，供合并步消费后清空 |
| `FunctionNode::fromLibrary` | `src/ast/ast.h` | bool 标志，默认 `false`。合并步对来自库的函数设为 `true`，visitor 据此走私有函数路径 |

#### 3.2.3 Parser

| 变更 | 文件 | 说明 |
|---|---|---|
| `parseLibrary()` | `src/parser/parser.cpp` | 解析 `Library Name:` 块；支持 dotted 库名；拒绝 `__init__` 和 `main` |
| `parseContract()` 扩展 | `src/parser/parser.cpp` | 顶层循环先吃零或多个 `Library` 块，再吃唯一 `Contract`，把库挂在 `ContractNode::libraries` 上 |

#### 3.2.4 Library 合并步 (Merge)

位于 `ASTToBytecodePass::execute()` 开头（`src/ast_to_bytecode_pass.h`），在所有 AST visitor 运行之前执行：

1. 遍历 `ContractNode::libraries`。
2. 对每个 `LibraryNode::members` 中的 `FunctionNode`，设置 `fromLibrary = true`。
3. 将所有库成员 **prepend** 到 `ContractNode::members`（保证库函数先于合约 `main` 被 visitor 登记）。
4. 清空 `libraries`。

合并后，后续的 `CollectSymbolsVisitor` / `StaticInfoVisitor` / `PreAnalysisVisitor` / `ASTToBytecodeVisitor` 看到的是一个"扩充后的 ContractNode"，与手写全部代码等价。

#### 3.2.5 Visitor 对库函数的处理

| Visitor | 行为 |
|---|---|
| `CollectSymbolsVisitor` | 正常做唯一性检查——库函数与用户函数重名会报 "duplicate function" 语义错误 |
| `StaticInfoVisitor` | `fromLibrary == true` 的函数视为私有，**不输出到 ABI JSON** |
| `PreAnalysisVisitor` | 正常分析（所有权检查等） |
| `ASTToBytecodeVisitor` | `fromLibrary == true` 的函数注册到 `m_privateFunctions` 表；用户合约 main 中调用时走**私有函数内联解析**路径 |

#### 3.2.6 字节码效果

库函数在调用点**被内联展开**为等价的 Bitcoin Script 操作码序列——与直接在合约内手写代码产出的字节码完全一致，无额外调用开销。

---

## 4. stdlib 目录结构

```
stdlib/
└── std/
    └── p2pkh.ct          # P2PKH 标准验证库
```

### 4.1 stdlib 根目录查找顺序

由 `ImportResolver::findStdlibRoot()`（`src/lexer/import_resolver.cpp`）按以下优先级定位：

1. 环境变量 `APC_STDLIB_PATH`
2. `user_preferences.json` 中的 `paths.stdlib` 配置项
3. CMake 编译时注入宏 `APC_DEFAULT_STDLIB_PATH`
4. 当前工作目录下的 `stdlib/`

### 4.2 现有库模板

#### `std.p2pkh` — P2PKH 标准验证

| 函数 | 签名 | 返回 | 说明 |
|---|---|---|---|
| `verifyP2PKH` | `(signature: hex, pubKey: hex)` | `bool` | 执行标准 P2PKH 验证：`EqualVerify(Hash160(pubKey), self.pubKeyHash)` 然后返回 `CheckSig(signature, pubKey)` 的布尔结果 |

**宿主合约契约**：使用本库的合约必须声明成员变量 `self.pubKeyHash`（20 字节 Hash160，由部署方替换占位符）。

等价 Bitcoin Script（与标准 BTC P2PKH 字节级一致）：
```
OP_DUP OP_HASH160 <self.pubKeyHash> OP_EQUALVERIFY OP_CHECKSIG
76   a9      <20-byte-pkh>          88           ac
```

---

## 5. 编写自定义 Library

### 5.1 文件模板

```python
# my_lib.ct
Library my.lib:

    Struct MyData:
        field1: hex
        field2: int

    def myHelper(arg1: hex, arg2: int):
        # 业务逻辑
        # 可直接引用 self.<field>, 但要求所有宿主合约声明同名成员
        ...
```

### 5.2 约束

| 规则 | 原因 |
|---|---|
| 不可定义 `def main(...)` | main 是合约入口，库不是合约 |
| 不可定义 `def __init__(...)` | 构造函数属于合约生命周期 |
| 函数名不可与用户合约函数重名 | 合并到同一命名空间后由 `CollectSymbolsVisitor` 检查 |
| 引用的 `self.<field>` 必须在宿主合约中声明 | 合并后符号解析失败会报 "undefined field"，构成隐式契约 |

**关于 `self.*` 的取舍**：
- **可用**：库函数内可直接写 `self.pubKeyHash` 等访问，产出的字节码与"在合约内手写"完全一致，可完美对齐 BTC 标准脚本字节序列。
- **代价**：库对宿主合约有隐式结构要求，降低了复用灵活性——使用库前需查阅其文档了解需要声明哪些 `self` 成员。
- **建议**：通用工具类库优先用参数传递；绑定特定协议（如 P2PKH/P2SH）的"模板合约"类库可使用 `self.*` 约定以获取最贴近标准的输出。

### 5.3 放置位置

- **标准库**：放入 `stdlib/` 对应 dotted path 目录（如 `stdlib/std/my_lib.ct` 对应 `import std.my_lib`）。
- **项目本地库**：放在项目目录内，使用相对路径导入（`import "./libs/my_lib"`）。

### 5.4 编译命令

```bash
# 指定 stdlib 路径
APC_STDLIB_PATH=$(pwd)/stdlib ./build/bin/utxo_Interpreter my_contract.ct

# 或在 user_preferences.json 中配置
{
  "paths": {
    "stdlib": "/path/to/stdlib"
  }
}
```

---

## 6. 实现效果

### 6.1 编译产出对比

**编译 `test/wallet.ct`：**
```bash
APC_STDLIB_PATH=$(pwd)/stdlib ./build/bin/utxo_Interpreter test/wallet.ct
```

产出 `wallet.json`：

| 字段 | 值 | 说明 |
|---|---|---|
| `abi` | `[{name: "main", params: [signature:hex, pubKey:hex]}]` | 仅包含用户合约的公共函数；库函数 `verifyP2PKH` **不暴露** |
| `lock.hex` | `76a9<self.pubKeyHash>88ac` | 与标准 BTC P2PKH (OP_DUP OP_HASH160 \<pkh\> OP_EQUALVERIFY OP_CHECKSIG) **字节级一致** |

### 6.2 sCrypt 对照

| 特性 | sCrypt | AtomicProof (Library 模型) |
|---|---|---|
| 导入语法 | `import { P2PKH } from 'scrypt-ts'` | `import std.p2pkh` |
| 复用方式 | `class MyWallet extends P2PKH` (继承) | `verifyP2PKH(sig, pk)` (函数调用 + 库内 `self.*` 约定) |
| 状态绑定 | `@prop() addr: PubKeyHash` | `self.pubKeyHash` (合约成员，部署时替换；库函数直接引用) |
| 入口函数 | `@method() public unlock(...)` | `def main(...)` |
| ABI 隔离 | 公共方法均导出 | 库函数不进入 ABI |
| 字节码策略 | 方法分发 | 调用点内联（零开销） |

### 6.3 与此前"整合约级"复用的对比

| 维度 | 此前 (Contract 级 import) | 现在 (Library 模型) |
|---|---|---|
| 模板形态 | 完整 `Contract P2PKH` (含 `main`) | `Library std.p2pkh` (仅 free `def` / `Struct`) |
| 能否与用户合约共存 | 不能——两段 `Contract` 冲突 | 可以——`Library` + `Contract` 共存 |
| 能否叠加业务逻辑 | 不能——`main` 已被模板占用 | 可以——用户写自己的 `main`，调用库函数组合 |
| ABI | 模板的 ABI | 用户合约的 ABI（库函数隐藏） |

---

## 7. 当前限制

| 限制 | 说明 | 计划 |
|---|---|---|
| Flat 命名空间 | 库函数直接合并到合约命名空间，不支持 `lib.fn()` 限定调用 | 后续可引入模块前缀 |
| 无继承 / extend | 不支持 `Contract X extends Y` | 库函数模型已覆盖多数场景 |
| 无 `export` / 可见性控制 | Library 内所有 `def` / `Struct` 均对外可见 | 按需引入 |
| 无多合约 | 一个翻译单元仍然只允许一个 `Contract` | 由设计决定 |

---

## 8. 相关文件索引

| 文件 | 角色 |
|---|---|
| `src/include/token_type.h` | `TOKEN_LIBRARY` 枚举定义 |
| `src/lexer/lexer.cpp` | `Library` 关键字注册 |
| `src/lexer/import_resolver.cpp` | import 源码内联（不变） |
| `src/ast/ast.h` | `LibraryNode`、`ContractNode::libraries`、`FunctionNode::fromLibrary` |
| `src/parser/parser.cpp` | `parseLibrary()`、`parseContract()` 多顶层块支持 |
| `src/ast_to_bytecode_pass.h` | Library 合并步 |
| `src/compiler/ast_to_bytecode_visitor.cpp` | 库函数 → 私有函数路径 |
| `src/compiler/static_info_visitor.cpp` | 库函数不进 ABI |
| `stdlib/std/p2pkh.ct` | 标准 P2PKH 库模板 |
| `test/wallet.ct` | 端到端使用示例 |
