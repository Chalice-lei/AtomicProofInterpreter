# 如何测试合约

---

在将合约部署到链上之前，充分测试所有执行路径是必要的。本节介绍用 UTXO_Compiler 提供的工具进行本地测试的方法，以及组织测试用例的实践建议。

---

## 测试策略概览

UTXO 合约的测试重点与以太坊合约有所不同。由于合约逻辑全在链上执行，本地测试的核心目标是：

1. **验证正常路径**：合法输入应使合约返回真（花费被允许）
2. **验证拒绝路径**：不合法输入应被合约拒绝（返回假或直接终止）
3. **验证边界条件**：恰好在条件边界上的值
4. **验证所有公有函数**：每条花费路径都独立测试
5. **验证字节码输出**：编译结果符合预期的操作码序列

---

## 方法一：编译器冒烟测试

最基础的测试是直接让编译器编译合约文件，确认没有语法错误和所有权错误：

```bash
./utxo_interpreter my_contract.ct
```

如果生成 `my_contract.json` 且没有报错，说明合约至少在语法、类型、所有权和字节码生成层面是正确的。输出 JSON 中的 `lock.hex` 是最终锁定脚本十六进制，`lock.asm` 是反汇编结果，`abi` / `functions` 可用于核对解锁参数顺序。

如果你维护的是一个包含多份合约的项目，建议用批处理脚本做快速冒烟检查：

```bash
# 逐一编译 contracts/ 目录下的合约
COMPILER="${COMPILER:-utxo_interpreter}"
for f in contracts/**/*.ct; do
    echo "Testing: $f"
    "$COMPILER" "$f" > /dev/null && echo "  OK" || echo "  FAILED"
done
```

本仓库已经按能力维度组织了一组 `.ct` 用例，可作为你写测试时的参考：

| 目录 | 覆盖内容 |
|------|----------|
| `test/test_basic_statement/` | 变量声明、赋值、算术和比较运算 |
| `test/test_function/` | 公有函数、私有函数、参数、返回值 |
| `test/test_array_and_struct/` | 结构体、数组、字段访问、结构体数组 |
| `test/test_builtin_function_and_object/` | 哈希、签名、栈操作、`BVM` / `self` |
| `test/contract_file/` | Counter、订单簿、预言机、借贷等合约级示例 |
| `scrypt_rewrites/` | 从 sCrypt 示例改写而来的对照合约 |

编译这些示例时，建议先从基础语法目录开始，再到 `test/contract_file/`。复杂交易级合约通常需要配套交易上下文，适合放到调试器里进一步验证。

---

## 方法二：调试器手动测试

[内置调试器](./how-to-debug-a-contract.md) 是目前最直接的执行验证手段。启动调试器，输入一组参数，让合约运行到结束，观察最终栈上的值是否为真：

```bash
./utxo_interpreter my_contract.ct --debug
```

### 测试矩阵

对于每个公有函数，建议按以下矩阵准备测试输入：

| 场景 | 预期结果 |
|------|---------|
| 正确的签名 + 匹配的公钥 | 栈顶 = 1，通过 |
| 错误的签名 | 执行终止或栈顶 = 0 |
| 公钥哈希不匹配 | `EqualVerify` 失败，终止 |
| 空数据 / 零值 | 视合约逻辑而定 |
| 边界值（如时间刚好等于截止时间） | 验证 `>=` / `>` 的语义 |

---

## 方法三：编写测试合约文件集

对于需要持续集成的项目，建议将测试用例组织为独立的 `.ct` 文件，放在 `tests/` 目录下，并配合 shell 脚本批量运行：

### 批量测试脚本

```bash
#!/bin/bash
# run_tests.sh

COMPILER="${COMPILER:-utxo_interpreter}"
PASS=0
FAIL=0

for f in tests/**/*.ct; do
    output=$($COMPILER "$f" 2>&1)
    if [ $? -eq 0 ]; then
        echo "PASS: $f"
        ((PASS++))
    else
        echo "FAIL: $f"
        echo "  Error: $output"
        ((FAIL++))
    fi
done

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
```

---

## 方法四：调试信息辅助测试

使用 `-d` 标志编译，生成调试信息文件，然后配合调试器检查每一行的执行情况：

```bash
utxo_interpreter my_contract.ct -d
```

调试信息包含源码行号到字节码偏移的映射，可以用来：

- 确认某段代码是否被执行
- 检查循环的展开是否正确
- 验证条件分支的跳转逻辑

如果要指定调试信息文件名：

```bash
utxo_interpreter my_contract.ct --debug-output my_contract.debug
```

如果合约在私有函数或 `if/else` 子作用域中使用 `SetAlt` / `SetMain`，编译器默认会拦截这类写法，以避免副栈顺序被隐式破坏。确认你的副栈协议正确后，可显式开启：

```bash
utxo_interpreter --asa my_contract.ct
```

`--asa` 是 `--allow-subscope-altstack` 的短写。

---

## 如何判断测试结果

编译器和调试器关注的通过标准略有不同：

| 阶段 | 通过标准 | 失败信号 |
|------|----------|----------|
| 编译 | 生成 `.json`，无错误日志 | 语法错误、类型错误、所有权错误、导入失败 |
| 字节码审查 | `lock.asm` 与预期操作码一致 | 参数顺序、哈希顺序、`Verify` 操作位置不符合预期 |
| 调试执行 | 最终栈顶为真，或关键 `EqualVerify` 全部通过 | `EqualVerify` / `CheckSigVerify` 终止，或栈顶为 `0` |
| 交易级验证 | `BVM.unlockingInput`、`BVM.outputsHash` 等上下文与构造交易匹配 | 父交易 txid 重建失败、输出哈希不一致、状态推进错误 |

---

## 常见测试陷阱

**陷阱一：所有权错误只在运行时触发**

所有权检查是编译期的——编译器会在 `utxo_interpreter` 阶段报错。测试时只需确认编译通过即可，不需要担心"运行时所有权错误"。

**陷阱二：`EqualVerify` 失败不返回假，而是终止**

`EqualVerify`、`CheckSigVerify` 等 `*Verify` 函数在失败时直接终止执行（等价于脚本执行失败）。测试拒绝场景时，这类函数触发的失败不会在栈上留下 `0`，而是让整个执行中止。调试器中会显示执行被终止。

**陷阱三：合约成员变量是字节码的一部分**

测试同一个合约逻辑的不同参数时，不需要重新编译，因为合约成员变量直接嵌在字节码里，需要用户手动进行替换。

**陷阱四：循环次数是编译期常量**

`Range()` 的参数必须是编译期可确定的值（字面量或常量表达式）。不能用变量控制循环次数。

---

## 下一步

- [解释器使用说明](./references/interpreter-user-manual.md) — 用 AST 解释器或字节码运行器做非交互式执行验证
- [如何调试合约](./how-to-debug-a-contract.md) — 用交互式调试器深入检查执行过程

---

[🇬🇧 English version](../en/how-to-test-a-contract.md)
