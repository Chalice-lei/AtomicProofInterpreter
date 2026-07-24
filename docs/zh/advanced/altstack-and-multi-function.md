# 副栈与多函数协作

---

副栈（alt stack）是比特币脚本执行模型原生提供的第二栈（对应 `OP_TOALTSTACK` / `OP_FROMALTSTACK`），UTXO_Compiler 将其暴露为 `SetAlt` / `SetMain`。本文聚焦它在合约开发中的常见用法，尤其是跨函数传递中间状态这一高频模式。

---

## 背景：副栈来源与协作场景

在 UTXO 模型中，花费一个合约 UTXO 时，解锁脚本通常会**按顺序**调用多个公有函数：前面的函数负责一部分校验，或从父交易、锁定脚本等上下文中解析出「下一步还要用到的量」；后面的函数再对当前交易的输出、哈希约束等做验证。这些函数在源码里是相互独立的定义，在 BVM 上却**共用同一条主栈**，函数与函数之间也没有类似普通语言那样的形参传递——但后一步往往又必须依赖上一步已经算过、验过的中间结果。

如果把所有这类结果都改由解锁脚本再次显式传入第二个函数，既冗长，也容易与链上真实数据不一致。更常见的做法是：第一个函数在校验通过的同时，用 `SetAlt` 把需要延续的状态压入**副栈**；第二个函数再用 `SetMain` 按约定顺序取回，继续完成验证。

因此，这里的重点不是“发明一个新通道”，而是把脚本原生副栈用于跨函数传递中间状态。下文「进阶模式：跨函数状态中继」将演示计数器合约如何在两阶段函数之间用副栈衔接父交易校验与当前输出校验。

---

## SetAlt 与 SetMain

### SetAlt

```python
SetAlt(variable)
```

根据内置函数定义，主栈与副栈可视作一段连续线性存储，`SetAlt` 用于将主栈中的指定目标变量迁移到副栈（对应 BVM 的 `OP_TOALTSTACK`），其余元素的相对顺序保持不变。

```python
amount = BinToNum(amountBytes)
SetAlt(amount)     # amount 进入副栈
# amount 仍可使用（但通常不建议再用，逻辑上它"已经交出去了"）
```

### SetMain

```python
SetMain(variable)
```

根据内置函数定义，`SetMain` 通过指定副栈中的目标位置来调整主/副栈边界（对应 BVM 的 `OP_FROMALTSTACK` 语义），使该位置及其左侧元素回到主栈侧。

```python
SetMain(amount)    # 从副栈取回，绑定到名称 'amount'
use(amount)        # 正常使用
```

> **后进先出（LIFO）**：副栈是一个标准栈，`SetMain` 取出的是最后压入的那个值。多个值压入和取出时顺序要对应。

---

## 基础模式：单函数内暂存

最简单的用法是在一个函数里，用副栈临时保存一个值，在中间操作完成后取回：

```python
def processWithLoop(items: Data[3]):
    # acc 需要在循环迭代间传递
    acc = Push(0)
    SetAlt(acc)

    for i in Range(2, -1, -1):
        ...
        SetMain(acc)                   # 取回上次迭代的累积值
        newAcc = Cat(items[i], acc)    # 拼接新数据
        SetAlt(newAcc)                 # 压回副栈
        ...
    # 使用 acc
    SetMain(acc)                       # 循环结束，取出最终结果
```

这个模式在需要在循环中"传递"一个正在累积的变量时非常常见。

---

## 进阶模式：跨函数状态中继

这是副栈在合约工程中的高频用途之一：函数 A 将计算结果压入副栈，函数 B 从副栈取出继续使用。下面用 `counter.ct` 的真实逻辑说明。

### 函数 A：读取父交易并写入状态（`getCountFromPreTX`）

```python
def getCountFromPreTX(pretx: PreTX):
    # ... 省略与 vout、pretx 输出相关的读取 ...
    pre_count = BinToNum(code_data.Slice(1, 8))
    SetAlt(pre_count)

    pre_code_partialhash = pretx.Outputs[vout_copy].LockingScript.PartialHash.Clone()
    SetAlt(pre_code_partialhash)
    pre_code_size = pretx.Outputs[vout_copy].LockingScript.Size.Clone()
    SetAlt(pre_code_size)

    # ... 省略父交易校验逻辑 ...
```

### 函数 B：读取状态并验证当前输出（`verifyCurrentTX`）

```python
def verifyCurrentTX(ctx: CurrentTX):
    # ... 循环中省略若干逻辑 ...
    SetMain(pre_code_size)
    if size == pre_code_size.Clone():
        SetMain(pre_code_partialhash)
        EqualVerify(pre_code_partialhash.Clone(), ctx.Outputs[i].LockingScript.PartialHash.Clone())
        SetMain(pre_count)
        ctx_count = BinToNum(ctx.Outputs[i].LockingScript.SuffixData.Clone().Slice(1, 8))
        EqualVerify(pre_count.Clone() + 1, ctx_count)

        # 若后续还要继续使用，重新压回副栈
        SetAlt(pre_count)
        SetAlt(pre_code_partialhash)
        SetAlt(pre_code_size)
```

### 顺序规则

压栈和出栈的顺序**必须镜像对称**：

```
压栈顺序（SetAlt）：pre_count → pre_code_partialhash → pre_code_size
出栈顺序（SetMain）：pre_code_size → pre_code_partialhash → pre_count
```

### 本地运行阶段链

`run` 支持用逗号按源码顺序选择多个公有函数，并在同一个 BVM 会话中
执行，从而保留函数之间的主栈和副栈状态：

```bash
./build/bin/utxo_Interpreter run contract.ct getCountFromPreTX,verifyCurrentTX <args...>
```

位置参数按函数列表顺序拼接。若后置函数依赖前置函数留下的副栈状态，
单独运行后置函数会报告“缺少前置阶段留下的栈状态”，此时应改用阶段链。

---

## 小结

| 操作         | BVM 指令          | 内置语义（简）                 | 典型场景                   |
| ------------ | ----------------- | ------------------------------ | -------------------------- |
| `SetAlt(v)`  | `OP_TOALTSTACK`   | 将主栈目标变量迁移到副栈       | 暂存变量、跨函数传递状态   |
| `SetMain(v)` | `OP_FROMALTSTACK` | 调整主/副栈边界并恢复到主栈侧 | 从副栈取回、接收跨函数状态 |

副栈是比特币脚本原生机制；在合约工程中，它常被用于实现复杂链上逻辑（多步验证、状态机、跨函数协议）中的中间状态管理。

---

## 下一步

- [内置函数参考](../references/builtin-functions.md) — 所有内置函数的完整说明

---

[🇬🇧 English version](../../en/advanced/altstack-and-multi-function.md)
