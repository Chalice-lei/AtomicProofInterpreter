# 所有权系统

---

所有权（Ownership）是 UTXO_Compiler 合约语言中最独特的设计。它决定了变量的生命周期，直接对应 BVM 栈值的"弹出"语义。理解这套规则，是写出正确合约的基础。

---

## 为什么存在所有权

BVM 是一台栈式虚拟机：变量对应栈上的值，函数调用对应弹出操作。大多数操作码在执行时会**消耗（弹出）**它的操作数：

```
栈状态：[sig] [pubKey]
执行 OP_CHECKSIG：
  → 弹出 pubKey 和 sig
  → 压入结果（1 或 0）
栈状态：[result]
```

`pubKey` 和 `sig` 在 `OP_CHECKSIG` 执行后，就从栈上消失了，不复存在。

如果你的合约代码在 `CheckSig` 之后又试图使用 `pubKey`，编译器会直接报错。

**所有权系统的作用**，就是让编译器在编译期就捕获这类错误，而不是等到链上执行时出问题。

---

## 核心规则

### 规则一：局部变量使用后被消耗

局部变量（函数参数或函数体内声明的变量）一旦出现在以下情形，就被"消耗"，之后不可再引用：

- 作为内置函数的参数
- 赋值给另一个变量
- 参与算术/比较运算

```python
def example(data: hex, key: hex):
    hash1 = Hash160(data)     # data 在此被消耗
    hash2 = Hash160(data)     # ❌ 编译错误：data 已被消耗
```

### 规则二：合约成员变量不受约束

`self.fieldName` 在编译期被替换为字节码中的常量，不占用运行时栈位置，因此可以无限次读取：

```python
def verify(sig1: hex, sig2: hex):
    r1 = CheckSig(sig1, self.ownerKey)   # self.ownerKey 是编译期常量
    r2 = CheckSig(sig2, self.ownerKey)   # ✓ 可以再次使用
```

### 规则三：字段访问消耗字段所有权

访问结构体实例的字段后，该字段被消耗（结构体本身仍然存在，只是该字段不能再访问）：

```python
def process(output: Output):
    val1 = output.Value          # output.Value 被消耗
    val2 = output.Value          # ❌ 错误：output.Value 已被消耗

    script = output.LockingScript   # output 的其他字段仍可访问
```

---

## Clone：变量复制

`Clone()` 是解决所有权冲突的标准手段。它对应 BVM 的 `OP_DUP` 指令：

```python
copy = original.Clone()
```

执行后：`original` 被消耗（`OP_DUP` 弹出栈顶并压入两个副本，但原来那个"逻辑位置"被消耗），`copy` 是新的独立副本。

### 正确的克隆模式

```python
# 需要用同一个值两次
def example(pubKey: hex):
    forHash = pubKey.Clone()      # 克隆一份给哈希用
    h = Hash160(forHash)          # forHash 被消耗
    ok = CheckSig(sig, pubKey)    # pubKey 被消耗（最后一次使用）
```

---

## SetAlt / SetMain：不消耗所有权的操作

`SetAlt` 和 `SetMain` 是合约语言中少有的**不消耗变量所有权**的操作：

```python
data = Push(0xdeadbeef)
SetAlt(data)     # data 移入副栈，但 data 这个绑定仍然有效
SetMain(data)    # 从副栈取回，data 仍然有效
use(data)        # 仍可使用
```

这两个操作主要用于跨函数传递状态，详见 [副栈与多函数协作](./altstack-and-multi-function.md)。

---

## Delete：显式清理

当你确定一个变量不再需要，用 `Delete` 显式销毁：

```python
{header, body} = Split(rawData, 4)
Delete(header)       # 不需要 header，清理掉
process(body)
```

`Delete` 接受多个参数：

```python
Delete(tmp1, tmp2, tmp3)
```

对结构体使用时递归删除所有字段：

```python
Delete(someStruct)   # 等价于逐一 Delete 每个字段
```

---

## Keep：标记保留

`Keep` 是零成本的标记操作，不生成任何字节码，仅告诉编译器"这些变量之后还会用到"：

```python
SetAlt(stateA)
SetAlt(stateB)
Keep(stateA, stateB)   # 告知编译器不要报"未使用变量"警告
```

---

## 常见错误与修复

### 错误一：函数参数被多次传入

```python
def bad(key: hex, sig1: hex, sig2: hex):
    CheckSig(sig1, key)    # key 被消耗
    CheckSig(sig2, key)    # ❌ key 已消耗
```

修复：

```python
def good(key: hex, sig1: hex, sig2: hex):
    keyCopy = key.Clone()
    CheckSig(sig1, keyCopy)
    CheckSig(sig2, key)
```

### 错误二：结构体字段被访问两次

```python
def bad(output: Output):
    v1 = BinToNum(output.Value)   # output.Value 被消耗
    v2 = BinToNum(output.Value)   # ❌
```

修复：

```python
def good(output: Output):
    rawVal = output.Value.Clone()
    v1 = BinToNum(rawVal)
    v2 = BinToNum(output.Value)
```

### 错误三：忘记解构赋值接收多返回值

```python
Split(data, 10)    # ❌ Split 返回两个值，必须用解构赋值接收
```

修复：

```python
{left, right} = Split(data, 10)   # ✓
```

### 错误四：在循环中重复消耗同一变量

```python
for i in Range(0, 3, 1):
    process(sharedData)    # ❌ 第一次迭代后 sharedData 就被消耗了
```

修复：将共享数据在循环外克隆足够份数，或改用 `self` 成员变量。

---

## 下一步

- [副栈与多函数协作](./altstack-and-multi-function.md) — SetAlt/SetMain 的完整使用模式

---

[🇬🇧 English version](../../en/advanced/ownership-system.md)
