# AtomicProof 解释器使用说明

本文说明 `utxo_interpreter` 的日常使用方式，覆盖编译、REPL、AST 解释器、字节码运行器、参数注入、交易上下文文件和调试入口。

---

## 快速开始

在项目根目录构建并查看版本：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
./build/bin/utxo_interpreter --version
```

编译 `.ct` 合约并生成 JSON：

```bash
./build/bin/utxo_interpreter -l none test/interpreter/ast_minimal_return.ct
```

直接用 AST 解释器运行函数：

```bash
./build/bin/utxo_interpreter -l none test/interpreter/ast_minimal_return.ct \
  --run-ast --function main --arg 41
```

编译后用字节码运行器执行函数：

```bash
./build/bin/utxo_interpreter -l none test/interpreter/ast_minimal_return.ct \
  --run-bytecode --function main --arg 41
```

进入交互式 Shell：

```bash
./build/bin/utxo_interpreter --shell
```

---

## 1. 准备可执行文件

从源码构建后，可执行文件默认位于 `build/bin/utxo_interpreter`：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j

./build/bin/utxo_interpreter --version
```

基本命令格式：

```bash
utxo_interpreter [options] filename.ct
```

输入文件必须是 `.ct` 合约文件。下面的示例都假设在项目根目录运行：

```bash
./build/bin/utxo_interpreter ...
```

如果当前是 Debug 构建，默认日志可能比较详细；需要安静输出时加 `-l warning`、`-l error` 或 `-l none`。

---

## 2. 选择运行模式

| 模式 | 命令 | 用途 |
|------|------|------|
| 只编译 | `utxo_interpreter contract.ct` | 生成锁定脚本 JSON，不执行 |
| 交互式 Shell | `--repl` / `--shell` | 进入解释器 REPL，直接输入表达式、语句或函数定义 |
| AST 解释 | `--run-ast` / `--interpret-ast` | 直接解释 AST 子集，适合快速验证函数返回值 |
| 字节码运行 | `--run-bytecode` / `--run` | 编译后在 BVM 模拟器中非交互式执行 |
| 交互式调试 | `--debug` | 进入 CLI 调试器，支持断点和单步 |
| 自测 | `--runtime-self-test`、`--ast-self-test` | 运行解释器内置自测 |

`--debug`、`--run-bytecode`、`--run-ast` 三种执行入口互斥，一次只能选择一种。`--repl` / `--shell` 也不能和这些执行入口一起使用；它可以不带文件启动，也可以带一个 `.ct` 文件用于预加载函数和结构体定义。

### 2.1 命令行选项速查

| 选项 | 适用场景 | 说明 |
|------|----------|------|
| `-h`、`--help` | 通用 | 显示帮助信息 |
| `-v`、`--version` | 通用 | 显示版本、目标架构和能力信息 |
| `-l <level>`、`--log-level <level>` | 通用 | 设置日志级别：`debug`、`info`、`warning`、`error`、`critical`、`none` |
| `--allow-subscope-altstack`、`--asa` | 编译 / 运行 | 允许在 `if/else`、私有函数等子作用域中使用 `SetAlt` / `SetMain` |
| `-d` | 编译 | 生成调试信息，但不启动调试器 |
| `--debug-output <file>`、`--debug-out <file>` | 编译 | 指定调试信息输出文件，并自动启用调试信息生成 |
| `--repl`、`--shell`、`repl`、`shell` | REPL | 启动交互式 Shell；可以不带文件，也可以带 `.ct` 文件预加载 |
| `--run-ast`、`--interpret-ast` | AST 解释 | 直接解释 AST 子集 |
| `--run-bytecode`、`--run` | 字节码运行 | 编译后用 BVM 模拟器非交互式执行 |
| `--debug` | 调试 | 编译并进入交互式调试器 |
| `--function <name>`、`--function=<name>` | AST / 字节码 | 指定入口函数；AST 模式可用逗号顺序执行多个函数 |
| `--arg <value>`、`--arg=<value>` | AST / 字节码 | 按参数顺序传入位置参数 |
| `--param <path=value>`、`--param=<path=value>` | AST | 设置函数命名参数、结构体字段或数组元素 |
| `--self <field=value>`、`--self=<field=value>` | AST | 设置合约实例字段 `self.<field>` |
| `--bvm <field=value>`、`--bvm=<field=value>` | AST | 设置运行时 `BVM.<field>` 字段或模拟签名结果 |
| `--txfile <file>`、`--txfile=<file>` | AST / 字节码 | 读取交易上下文；AST 模式还可读取 `self` 和函数参数 |
| `--runtime-self-test` | 自测 | 运行解释器运行时自测 |
| `--ast-self-test` | 自测 | 运行 AST 解释器自测 |

---

## 3. 只编译合约

```bash
./build/bin/utxo_interpreter -l none test/interpreter/ast_minimal_return.ct
```

成功后会在当前工作目录生成同名 JSON，例如 `ast_minimal_return.json`。主要字段包括：

| 字段 | 说明 |
|------|------|
| `metadata` | 编译器名称、版本、源文件等信息 |
| `abi` | 公有函数及参数列表 |
| `lock.asm` | 锁定脚本反汇编 |
| `lock.hex` | 锁定脚本十六进制 |
| `unlock` | 解锁参数模板 |
| `functions` | 公有/私有函数摘要 |

生成调试信息但不进入调试器：

```bash
./build/bin/utxo_interpreter -l none test/interpreter/ast_minimal_return.ct \
  --debug-output /tmp/ast_minimal_return.debug
```

`--debug-output` 会自动启用调试信息生成；只需要启用调试信息但不指定输出路径时，也可以使用 `-d`。调试信息文件是 JSON 文本，包含函数范围、源码行号和字节码 PC 映射等数据。

如果合约需要在 `if/else`、私有函数等子作用域中使用 `SetAlt` / `SetMain`，编译或运行时加上 `--asa`：

```bash
./build/bin/utxo_interpreter -l none --asa contract.ct
```

---

## 4. AST 解释器

AST 解释器不走 BVM 字节码执行，而是直接解释已解析的 AST。它适合快速验证普通表达式、分支、循环、结构体字段、`self` 和 `BVM` 上下文访问。

### 4.1 运行单个函数

```bash
./build/bin/utxo_interpreter test/interpreter/ast_minimal_return.ct \
  --run-ast --function main --arg 41
```

输出示例：

```text
AST interpretation result
  status: finished
  function: main
Return values (1)
  [0] 43 hex=0x2b int=43
Error: <none>
```

如果省略 `--function`，AST 解释器会选择第一个非库、非私有的公有函数。

### 4.2 参数输入格式

`--arg` 按函数参数顺序传入位置参数：

```bash
--arg 42
--arg -1
--arg 0x1234abcd
--arg true
--arg false
--arg '"hello"'
```

解析规则：

| 输入 | 解释 |
|------|------|
| `42`、`-1` | 整数 |
| `true`、`false` | 布尔值 |
| `0x...` | 十六进制字节 |
| `"hello"` | 字符串 |
| 空字符串 | 按声明类型生成默认值 |

对 `hex` 类型参数，未带 `0x` 的值也会按十六进制处理。

### 4.3 命名参数和嵌套字段

`--param` 用 `字段路径=值` 的方式设置函数参数，适合结构体和数组：

```bash
./build/bin/utxo_interpreter test/interpreter/ast_tx_context.ct \
  --run-ast \
  --function main \
  --bvm unlockingInput=0x010203 \
  --param 'pretx.Outputs[1].LockingScript.Size=0x03000000' \
  --param 'pretx.Outputs[1].Value=0x2a000000'
```

字段路径支持：

| 写法 | 示例 |
|------|------|
| 普通字段 | `pretx.version=1` |
| 嵌套字段 | `pretx.Outputs[1].LockingScript.Size=0x03000000` |
| 数组下标 | `outs[1].Value=0x2a000000` |

命名参数会按合约声明的结构体类型自动补齐缺省字段。没有提供的字段使用类型默认值。

### 4.4 `self` 和 `BVM` 字段

合约中的 `self.xxx` 用 `--self` 设置：

```bash
--self pubKeyHash=0x00112233445566778899aabbccddeeff00112233
```

合约中的 `BVM.xxx` 用 `--bvm` 设置：

```bash
--bvm unlockingInput=0x010203
--bvm checkSigResult=true
```

AST 模式下，`CheckSig`、`CheckSigVerify`、`MultiSig`、`MultiSigVerify` 不会执行真实签名验证。需要用 `--bvm checkSigResult=true/false` 或 `--bvm multiSigResult=true/false` 显式指定结果。

### 4.5 顺序运行多个函数

`--function` 可以传入逗号分隔的函数名：

```bash
./build/bin/utxo_interpreter contract.ct \
  --run-ast --function setup,verify
```

多个函数会按顺序执行。位置参数 `--arg` 只用于第一个函数；后续函数建议通过 `--param` 或 `--txfile` 提供命名参数，未提供的参数使用默认值。

---

## 5. 字节码运行器

字节码运行器会先编译合约，再在 BVM 模拟器中非交互式运行选定函数。

```bash
./build/bin/utxo_interpreter test/interpreter/ast_minimal_return.ct \
  --run-bytecode --function main --arg 41
```

输出示例：

```text
Bytecode run result
  status: finished
  compile: success
  function: main [pc 0, 3)
  pc: 3 / 64
  executed: 3 instruction(s)
  max_stack: 1, max_call_depth: 1
Main stack (1, bottom -> top)
  [0] 0x2b (int=43)
Alt stack (0, bottom -> top)
  <empty>
Error: <none>
```

参数规则：

| 项 | 行为 |
|----|------|
| `--function <name>` | 指定执行函数；省略时默认选择第一个公有函数 |
| `--arg <value>` | 按参数顺序压入主栈 |
| 参数缺失 | 使用默认值 `0x00`，并在输出中给出 warning |
| 参数过多 | 返回 `argument_error` |
| 结构体参数 | 按编译输出中的字段展开顺序压栈 |

注意：非交互式字节码运行器当前不能从 CLI 配置真实签名验证回调。如果字节码包含 `CHECKSIG` / `CHECKMULTISIG`，运行器会给出警告，并在执行到签名校验时拒绝默认通过。验证签名路径时，优先使用 AST 模式的 `--bvm checkSigResult=...`，或进入交互式调试器。

字节码运行器也支持 `--txfile`，但只消费其中的 BVM 交易元数据，例如 `version`、`locktime`、`inputCount`、`outputCount`、`inputsHash`、`unlockingInput`、`outputsHash`。函数参数仍通过 `--arg` 传入。

`--run-bytecode` 依赖调试器/BVM 模拟器组件；如果构建时关闭了 `BUILD_DEBUGGER`，会提示 `--run-bytecode requires BUILD_DEBUGGER=ON`。

---

## 6. 交易上下文文件

`--txfile <file>` 可同时用于 AST 解释器和字节码运行器，但两者消费的数据范围不同：

| 模式 | 使用范围 |
|------|----------|
| AST 解释器 | 可提供 `BVM` 元数据、`self` 字段和函数参数 |
| 字节码运行器 | 只用于设置 BVM 交易元数据，函数参数仍使用 `--arg` |

### 6.1 文本格式

文本文件每行一个 `key:value` 或 `key=value`，支持 `#` 注释。AST 模式支持下面所有写法；字节码运行器的文本格式只读取传统 BVM 键，并建议使用冒号分隔：

```text
# BVM 元数据
version: 1
locktime: 0
inputCount: 1
outputCount: 2
unlockingInput: 0x010203
outputsHash: 0x000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f

# 函数参数路径
pretx.Outputs[1].LockingScript.Size: 0x03000000
pretx.Outputs[1].Value: 0x2a000000
```

AST 模式下，还可以使用显式前缀：

```text
bvm.unlockingInput=0x010203
self.pubKeyHash=0x00112233445566778899aabbccddeeff00112233
param.pretx.Outputs[1].Value=0x2a000000
```

运行示例：

```bash
./build/bin/utxo_interpreter test/interpreter/ast_tx_context.ct \
  --run-ast --function main --txfile test/interpreter/ast_tx_context.txt
```

输出中的返回值为 `45`，即 `0x2a` 加上 `BVM.unlockingInput` 第三个字节 `0x03`。

### 6.2 JSON 格式

JSON 文件支持顶层 `bvm`、`self`、`params`：

```json
{
  "bvm": {
    "unlockingInput": "0x010203",
    "checkSigResult": true
  },
  "self": {
    "pubKeyHash": "0x00112233445566778899aabbccddeeff00112233"
  },
  "params": {
    "pretx": {
      "Outputs": [
        {},
        {
          "Value": "0x2a000000",
          "LockingScript": {
            "Size": "0x03000000"
          }
        }
      ]
    }
  }
}
```

JSON 还可以使用 `currenttx` / `currentTx` / `ctx`、`pretx` / `preTx` / `previousTx` 等交易对象名。对于标准的 `inputs` / `outputs` 结构，解释器会尝试派生：

| 派生项 | 来源 |
|--------|------|
| `inputCount`、`outputCount` | `inputs` / `outputs` 数组长度 |
| `inputsHash` | 输入数据拼接后 SHA-256 |
| `outputsHash` | 输出金额与脚本哈希拼接后 SHA-256 |
| `pretx.outputs[i].value` | 输出金额字段 |
| `pretx.outputs[i].lockingScript.*` | 输出锁定脚本字段 |
| `txData`、`txid` | 可用交易 preimage 派生，或使用显式 `txid` |
| `unlockingInput` | 当前输入的解锁数据或由输入字段派生 |

字段名匹配会忽略大小写和常见分隔符，并支持一些别名，例如 `amount` / `satoshis` 可映射到 `Value`。

---

## 7. 交互式 Shell

启动 REPL：

```bash
./build/bin/utxo_interpreter --shell
```

也可以启动时预加载一个合约文件中的函数和结构体：

```bash
./build/bin/utxo_interpreter test/interpreter/ast_minimal_return.ct --shell
```

REPL 启动后会出现 `In [1]:` 提示符，可以直接输入表达式或语句：

```text
In [1]: 1 + 2
Out[1]: 3
```

常用命令：

| 命令 | 说明 |
|------|------|
| `help` | 显示 REPL 帮助 |
| `history` | 查看已执行的输入单元 |
| `clear` | 清屏 |
| `%run <file.ct>` / `%load <file.ct>` | 加载合约文件中的函数和结构体到当前会话 |
| `%debug [file.ct]` | 进入现有 CLI 调试器；不带参数时调试最近加载的 `.ct` 文件 |
| `%who` | 查看当前会话中的变量、函数和结构体名 |
| `%reset` | 清空当前会话 |
| `exit` / `quit` | 退出 REPL |

函数、结构体、`if`、`for` 等块语句以空行结束。REPL 会保留同一会话中的全局定义，适合快速验证小段代码和表达式行为。`%debug` 会暂时切入调试器 REPL，退出调试器后回到解释器 Shell。

### 7.1 示例：空 Shell 中计算表达式并定义函数

下面的示例从空 Shell 启动，依次演示表达式求值、变量赋值、函数定义、函数调用、查看当前名称和查看历史记录。函数块输入完成后需要追加一个空行：

```bash
./build/bin/utxo_interpreter --shell -l none <<'EOF'
1 + 2
x = 5
x + 7
def inc(n: int):
    Return(n + 1)

inc(x)
%who
history
exit
EOF
```

关键输出类似：

```text
Out[1]: 3
Out[3]: 12
Out[5]: 6
inc  x
1: 1 + 2
2: x = 5
3: x + 7
4: def inc(n: int):
       Return(n + 1)
5: inc(x)
Bye.
```

### 7.2 示例：在 Shell 内加载合约文件

`%run` 和 `%load` 可以把 `.ct` 文件里的合约函数和结构体加载到当前会话。加载后可以直接调用其中的函数：

```bash
./build/bin/utxo_interpreter --shell -l none <<'EOF'
%run test/repl/repl_load.ct
double(4)
exit
EOF
```

输出示例：

```text
Loaded contract ReplLoad from test/repl/repl_load.ct
Out[1]: 8
Bye.
```

### 7.3 示例：启动时预加载合约文件

也可以把 `.ct` 文件放在命令行中，让 Shell 启动时自动加载：

```bash
./build/bin/utxo_interpreter test/repl/repl_load.ct --shell -l none <<'EOF'
double(6)
exit
EOF
```

输出示例：

```text
Loaded contract ReplLoad from test/repl/repl_load.ct
Out[1]: 12
Bye.
```

### 7.4 示例：完整合约文件

下面是一个可直接在 `--shell` 中加载的完整合约示例，文件位于 `examples/interpreter_usage/shell_contract.ct`。它包含结构体、私有辅助函数、条件分支、循环和多个可交互调用的入口函数：

```text
Contract ShellEscrowDemo:
    Struct Quote:
        UnitPrice: int
        Count: int
        Fee: int

    def _subtotal(unitPrice: int, count: int):
        Return(unitPrice * count)

    def _discount(subtotal: int, vip: bool):
        if vip:
            Return(subtotal / 10)
        else:
            Return(0)

    def quote(unitPrice: int, count: int, fee: int, vip: bool):
        subtotal: int = _subtotal(unitPrice, count)
        discount: int = _discount(subtotal.Clone(), vip)
        Return(subtotal - discount + fee)

    def canUnlock(paid: int, required: int):
        if paid >= required:
            Return(true)
        else:
            Return(false)

    def sampleTotal():
        quotes: int[] = [12, 18, 25]
        total: int = 0

        for i in Range(0, 3):
            total = total + quotes[i]

        Return(total)
```

在 Shell 中加载并调用：

```bash
./build/bin/utxo_interpreter examples/interpreter_usage/shell_contract.ct --shell -l none <<'EOF'
%who
quote(100, 3, 5, true)
canUnlock(275, 275)
canUnlock(200, 275)
sampleTotal()
exit
EOF
```

输出示例：

```text
Loaded contract ShellEscrowDemo from examples/interpreter_usage/shell_contract.ct
Quote  _discount  _subtotal  canUnlock  quote  sampleTotal
Out[1]: 275
Out[2]: true
Out[3]: false
Out[4]: 55
Bye.
```

---

## 8. 交互式调试器

进入调试器：

```bash
./build/bin/utxo_interpreter contract.ct --debug
```

调试器会先编译源码，然后进入 REPL。可用命令包括 `break`、`run`、`step`、`next`、`stack`、`bytecode`、`settxfile`、`showtx` 等。完整说明见 [调试器用户使用手册](./debugger_user_manual.md)。

---

## 9. 内置自测

运行解释器运行时自测：

```bash
./build/bin/utxo_interpreter --runtime-self-test
```

运行 AST 解释器自测：

```bash
./build/bin/utxo_interpreter --ast-self-test
```

这两个命令不需要传入 `.ct` 文件。

---

## 10. 常见问题

**提示 `File must be in .ct format`**

输入文件扩展名必须是 `.ct`。

**AST 模式提示 `function 'xxx' is not defined`**

检查 `--function` 名称是否和合约中的函数名一致。多个函数用英文逗号分隔，例如 `setup,verify`。

**AST 模式提示签名验证需要 `BVM.checkSigResult`**

在命令中加入：

```bash
--bvm checkSigResult=true
```

或在 JSON `txfile` 的 `bvm` 对象中设置 `checkSigResult`。

**`--run-bytecode` 提示需要 `BUILD_DEBUGGER=ON`**

重新配置构建：

```bash
cmake -S . -B build -DBUILD_DEBUGGER=ON
cmake --build build -j
```

**编译 import 标准库失败**

确认 `stdlib/` 在可执行文件可发现的位置，或显式设置：

```bash
export APC_STDLIB_PATH=/path/to/AtomicProofInterpreter/stdlib
```

**控制台日志过多**

使用日志级别参数：

```bash
./build/bin/utxo_interpreter -l none contract.ct --run-ast --function main
```

---

## 11. 相关文档

- [如何测试合约](../how-to-test-a-contract.md)
- [如何调试合约](../how-to-debug-a-contract.md)
- [调试器用户使用手册](./debugger_user_manual.md)
- [语言规范](./language-specification.md)
- [内置函数参考](./builtin-functions.md)
