# 示例合约与常见模式

---

本页把仓库中的 `.ct` 示例按学习路径整理出来。读完 [教程一：P2PKH](./tutorial-1-p2pkh.md) 和 [教程二：Counter](./tutorial-2-counter.md) 后，可以按这里的顺序继续看代码。

---

## 1. 基础语法示例

基础示例位于 `test/` 目录，适合用来确认某个语法点的最小写法。

| 目录 | 建议关注 |
|------|----------|
| `test/test_basic_statement/test_variable_declaration/` | `int`、`bool`、`hex`、`string` 的声明与初始化 |
| `test/test_basic_statement/test_assignment_statement/` | 字面量赋值、变量赋值、函数返回值赋值 |
| `test/test_basic_statement/test_operator/` | `+`、`-`、`*`、`/`、比较运算符 |
| `test/test_function/` | 公有函数、私有函数、函数参数、返回值 |
| `test/test_loop/` | `Range(start, stop, step)` 固定循环 |
| `test/test_array_and_struct/` | 结构体、数组、链式字段访问 |

推荐做法是直接编译单个文件：

```bash
./utxo_Interpreter test/test_basic_statement/test_operator/test_operator_add/operator_add_two_operands.ct
```

这些示例的价值在于“短”：当你不确定某个语法是否被当前编译器支持时，先找对应最小例子，再迁移到自己的合约中。

---

## 2. 标准库模板

标准库位于 `stdlib/`。当前最常用的是：

| 文件 | 用途 |
|------|------|
| `stdlib/std/p2pkh.ct` | 标准 P2PKH 签名验证模板 |
| `stdlib/std/schnorr.ct` | Schnorr 验证相关辅助函数 |

使用方式：

```python
import std.p2pkh

Contract Wallet:
    def main(signature: hex, pubKey: hex):
        ok = verifyP2PKH(signature, pubKey)
        Return ok
```

库函数可以访问宿主合约的 `self.X` 字段。例如 `std.p2pkh` 约定宿主提供 `self.pubKeyHash`。这类字段不会在库里声明，而是在部署实例化阶段写入锁定脚本。

---

## 3. 交易级合约示例

`test/contract_file/` 放的是更接近真实链上逻辑的合约。它们通常会建模父交易、当前交易输出和脚本尾部数据。

| 文件 | 展示模式 |
|------|----------|
| `counter.ct` | 从父交易读取旧状态，验证当前输出状态递增 |
| `price_oracle.ct` | 读取预言机相关数据并验证输出哈希 |
| `collateral_lending.ct` | 借贷场景下的多方输出约束 |
| `orderBook.ct` / `orderBook_*.ct` | 订单簿、找零、税费、卖方/买方输出组合 |
| `OrderBookSell.ct` / `orderBookSell*.ct` | 卖单路径和 FT/TBC 输出验证 |

阅读这些文件时，建议按三步拆解：

1. 先看 `Struct`，确认交易和脚本数据的字节布局；
2. 再看第一个公有函数，通常负责重建父交易或读取旧状态；
3. 最后看 `BVM.outputsHash`、`BVM.unlockingInput` 附近的 `EqualVerify`，这些往往是交易级安全约束。

---

## 4. 从 sCrypt 改写的示例

`scrypt_rewrites/` 中的合约来自 sCrypt boilerplate 中适合当前编译器表达的例子，便于对照两种语言的写法。

| 文件 | 适合学习 |
|------|----------|
| `helloWorld.ct` | 最小哈希/字符串验证 |
| `hashLock.ct` | 哈希锁 |
| `timeLock.ct` | 时间锁 |
| `multiSigPayment.ct` | 多签支付 |
| `accumulatorMultiSig.ct` | 累加器式多签 |
| `atomicSwap.ct` | 原子交换 |
| `coinToss.ct` | 双方随机数/哈希承诺 |
| `matrix2x2.ct` | 固定数组和固定循环 |
| `modExp.ct` | 算术计算 |

需要注意一个差异：sCrypt 可以有多个 `public` method，而当前编译器会把所有不以下划线开头的函数按声明顺序串行执行。因此改写多路径合约时，通常使用一个公有入口加 `path` 参数：

```python
def main(path: int, a: hex, b: hex):
    if path == 0:
        _pathA(a)
    else:
        _pathB(b)
```

---

## 5. 选择示例的经验

如果你要写钱包或身份验证合约，从 `stdlib/std/p2pkh.ct` 和 `tutorial-1-p2pkh.md` 开始。

如果你要写有状态 UTXO 合约，从 `test/contract_file/counter.ct` 开始，重点理解 `SuffixData`、父交易 txid 重建和 `BVM.outputsHash`。

如果你要迁移 sCrypt 合约，从 `scrypt_rewrites/README.md` 和同目录中的短合约开始，先把多 public method 改成单入口分支，再处理 `self` 数据实例化。

---

## 下一步

- [如何编写合约](../how-to-write-a-contract.md) — 回查语法和类型
- [所有权系统](../advanced/ownership-system.md) — 理解为什么需要 `Clone`
- [副栈与多函数协作](../advanced/altstack-and-multi-function.md) — 阅读复杂状态合约前的必备背景

---

[🇬🇧 English version](../../en/tutorials/examples-and-patterns.md)
