# 如何编写合约

---

本节系统介绍 UTXO_Compiler 合约语言的完整语法。如果你更喜欢从例子入手，可以先看 [教程一](./tutorials/tutorial-1-p2pkh.md)，再回来查阅本节作为参考。

---

## 1. 合约结构

每个 `.ct` 文件只能定义**一个合约**，基本骨架如下：

```python
import std.p2pkh      # 可选：导入标准库或相对路径库

Contract 合约名:
    # 结构体定义（可选，可有多个）
    Struct 结构体名:
        字段名: 类型

    # 公有函数（至少一个，作为合约的花费入口）
    def 函数名(参数: 类型):
        ...

    # 私有辅助函数（可选，以下划线开头）
    def _辅助函数名(参数: 类型):
        ...
```

合约名、函数名、字段名均为标识符，规则与 Python 一致：字母或下划线开头，后跟字母、数字或下划线。

### Library 与 import

`.ct` 文件可以在合约前使用 `import` 复用库。导入方式有两类：

```python
import std.p2pkh          # 从 stdlib/std/p2pkh.ct 加载
import "./lib/math.ct"    # 从当前文件目录加载相对路径
```

库文件本身使用 `Library` 定义，只能包含 `Struct` 和普通函数，不能定义 `Contract` 的入口函数：

```python
Library std.p2pkh:
    def verifyP2PKH(signature: hex, pubKey: hex):
        EqualVerify(Hash160(pubKey.Clone()), self.pubKeyHash)
        result = CheckSig(signature, pubKey)
        return result
```

导入后，库成员会被编译期合并进当前合约，库函数不会作为 ABI 中的公有花费入口暴露。库函数可以访问宿主合约的 `self.X` 字段；这是一种约定，宿主合约必须在部署时为这些占位符提供实际数据。

```python
import std.p2pkh

Contract Wallet:
    def main(signature: hex, pubKey: hex):
        ok = verifyP2PKH(signature, pubKey)
        Return ok
```

> `import` 会做循环导入检测，重复导入同一个库时只展开一次。标准库路径可通过 `APC_STDLIB_PATH` 或 `user_preferences.json` 中的 `paths.stdlib` 配置。

---

## 2. 数据类型

### 基础类型

| 类型           | 描述               | 字面量示例                            |
| -------------- | ------------------ | ------------------------------------- |
| `int`,`number` | 整数（BVM 大整数） | `0`, `42`, `-100`                     |
| `string`       | 字节字符串         | `"hello"`, `"world"`                  |
| `hex`          | 十六进制字节数组   | `0x1234`, `0xdeadbeef`                |
| `bool`         | 布尔值             | `1`（真）, `0`（假）                  |
| `address`      | 比特币 P2PKH 地址  | `"1RainRzqJtJxHTngafpCejDLfYq2y4KBc"` |

`address` 类型只支持标准 Base58 P2PKH 地址（以 `1` 开头，34 个字符）。`hex` 和 `string` 底层都是字节序列，区别仅在字面量书写形式。

### 定长 hex 类型

在结构体字段中，可以使用 `hexN` 声明固定字节长度的十六进制字段，常用于描述比特币交易的固定格式字段：

```python
Struct TxInput:
    txid:     hex32   # 32 字节交易 ID
    vout:     hex4    # 4 字节输出索引
    sequence: hex4    # 4 字节序列号
```

### 数组类型

结构体字段支持固定长度数组 `Type[N]`：

```python
Struct Transaction:
    Inputs:  TxInput[3]    # 3 个输入
    Outputs: TxOutput[3]   # 3 个输出
```

数组下标访问：`tx.Inputs[0]`，下标一般为整型字面量；若下标为整形变量，则一般与结构体数组字段组合使用，取到对应的结构体字段。

```python
    vout = BinToNum(BVM.unlockingInput.Slice(32, 4)) #获取code在父交易中输出的位置
    vout_copy = vout.Clone()
    #取到code_data
    code_data = pretx.Outputs[vout_copy].LockingScript.SuffixData.Clone()
```

### `uint64[]` 数组

除了结构体数组外，合约里也常用 `uint64[]`（写作 `uint64[N]`）来表达定长 64 位无符号整数数组，未使用时数组字段作为整体占据一个栈高，需要使用时通过 `OP_ROLL` 移动到栈顶，然后通过多次 `OP_SPLIT` 拆分为独立元素。

```python
    amount: uint64[6] = temp_data.Slice(3, 48)
    ft_amount_tax = Push(0)
    for i in Range(5, -1, -1):
        ft_amount_tax = BinToNum(amount[i]) + ft_amount_tax
```

使用要点：

- `uint64[N]` 中的 `N` 必须是编译期可确定的固定长度；
- 元素访问使用 `arr[i]`，索引建议配合 `Range` 按固定边界遍历；
- `uint64` 元素按 8 字节处理，适合金额、计数器、索引等整数序列场景。

如果你需要在结构体中表达一组计数值，也可以直接声明：

```python
Struct BatchData:
    counts: uint64[4]
```

### 内联匿名结构类型

临时使用的复合字段，可以不单独定义结构体，直接内联：

```python
utxoData: {txid: hex32, vout: hex4, sequence: hex4}
utxoData = Push(BVM.unlockingInput)
vout = BinToNum(utxoData.vout)
```

---

## 3. 变量

### 赋值

```python
count = 10
result = Hash160(pubKey)     # 将函数返回值绑定到变量
```

局部变量（除数组和内联匿名结构类型外）可直接赋值使用，结构体字段和函数参数必须声明类型。

### 合约成员变量（self）

合约成员变量可直接使用，在编译期会被替换为字节码中的固定常量，可在公有函数和私有函数中多次读取，**不受所有权限制**：

```python
Contract P2PKH:
    def verify(sig: hex, pubKey: hex):
        pubKey_copy = pubKey.Clone()
        pubKeyHash = Hash160(pubKey_copy)
        EqualVerify(pubKeyHash, self.pubKeyHash)
        result = CheckSig(sig, pubKey)
```

当前推荐的实例化方式是直接在合约中引用 `self.X`，由部署阶段替换为真实字节数据。虽然解析器保留了 `__init__` 形式用于部分测试和导出信息，入门合约里不建议依赖构造函数语义；实际部署请按 [如何部署与调用合约](./how-to-deploy-and-call-a-contract.md) 中的 `SuffixData` 流程处理。

---

## 4. 运算符

### 算术运算

```python
sum   = a + b
diff  = a - b
prod  = a * b
quot  = a / b   # 整除
```

取模没有对应运算符，使用内置函数 `Mod(a, b)`。

### 比较运算

```python
a == b    # 等于
a != b    # 不等于
a <  b    # 小于
a >  b    # 大于
a <= b    # 小于等于
a >= b    # 大于等于
```

比较运算返回整数 `1`（真）或 `0`（假），可直接用于 `if` 条件或 `Return`。

### 逻辑运算

合约语言**没有** `and` / `or` / `not` 关键字，逻辑运算统一用内置函数：

```python
ok = And(condition1, condition2)   # 逻辑与
ok = Or(condition1, condition2)    # 逻辑或
ok = Not(condition)                # 逻辑非
```

---

## 5. 控制流

### 条件语句

```python
if amount > threshold:
    CheckSigVerify(sig, pubKey)
else:
    Return (1 == 0)   # 拒绝
```

`if`,`else` 分支需成对出现，多分支需嵌套 `if`：

```python
if role == 1:
    _handleBuyer(sig, pubKey)
else:
    if role == 2:
        _handleSeller(sig, pubKey)
    else:
        Return (1 == 0)
```

### 循环语句

循环使用 `for ... in Range(start, stop, step)` 形式，语义类似 Python 的 `range`，但参数顺序是 `(start, stop_exclusive, step)`：

```python
# 从 2 递减到 0（包含 0）
for i in Range(2, -1, -1):
    data = Cat(items[i], data)

# 从 0 递增到 2（包含 2）
for i in Range(0, 3, 1):
    total = Add(total, values[i])
```

循环次数在编译期必须确定，主要用于遍历固定大小的数组或执行固定次数的操作。

---

## 6. 函数

### 公有函数（花费入口）

不以 `_` 开头的函数是**公有函数**，会出现在 ABI 中，并按**源码声明顺序**串行执行：

```python
Contract MultiPath:
    # 第一步：先验证父交易
    def loadPreviousState(pretx: PreTX):
        ...

    # 第二步：再验证当前交易
    def verifyCurrent(ctx: CurrentTX):
        ...
```

这与很多智能合约 SDK 中“调用某一个 public method”的模型不同。当前编译器会把所有公有函数视为同一条锁定脚本中的连续花费流程；如果你想表达多条互斥路径，常见写法是在单个公有入口里加入 `path` 参数，再用 `if/else` 分发。

```python
def main(path: int, sig: hex, pubKey: hex):
    if path == 0:
        _spend(sig, pubKey)
    else:
        _refund(sig, pubKey)
```

这个约定也解释了 `scrypt_rewrites/` 目录中的示例：从 sCrypt 多个 public method 改写而来时，通常会收敛成一个带 `path` 参数的入口。

### 私有辅助函数

以 `_` 开头的函数只能在合约内部调用，用于封装重复逻辑：

```python
def _verifyOwner(sig: hex, pubKey: hex, expectedHash: hex):
    pubKeyCopy = pubKey.Clone()
    pubKeyHash = Hash160(pubKeyCopy)
    EqualVerify(pubKeyHash, expectedHash)
    CheckSigVerify(sig, pubKey)

def spend(sig: hex, pubKey: hex, ownerHash: hex):
    ...
    _verifyOwner(sig, pubKey, ownerHash)
    ...
    Return (1 == 1)
```

### 返回语句

`Return`（大写）将表达式结果压入执行栈并生成"OP_RETURN"：

```python
Return (1 == 1)               # 永远通过
Return (1 == 0)               # 永远拒绝
Return CheckSig(sig, pubKey)  # 签名验证结果作为返回值
Return And(cond1, cond2)      # 组合条件
```

小写 `return` 用于私有函数将值返回给调用方：

```python
def _computeHash(data: hex):
    result = Hash160(data)
    return result
```

---

## 7. 结构体

结构体描述复合数据的字节布局，通常对应交易数据中某一段的格式：

```python
Struct Script:
    SuffixData:  string
    PartialHash: string
    Size:        int

Struct Output:
    Value:         int
    LockingScript: Script    # 结构体可以嵌套

Struct PreTX:
    VLIO:                string
    Inputs:              Input[3]
    UnlockingScriptHash: string
    Outputs:             Output[3]
```

结构体字段访问通过 `.` 操作符，支持多级链式访问：

```python
scriptSize = pretx.Outputs[0].LockingScript.Size
```

> **注意**：字段访问会消耗该字段的所有权。访问同一字段两次，需要在第一次前先 `.Clone()`。详见 [所有权系统](./advanced/ownership-system.md)。

---

## 8. 解构赋值

`{}` 语法用于接收返回多个值的函数的结果，或初始化结构体：

```python
# 接收 Split 的两个返回值
{header, body} = Split(rawData, 4)

# 接收私有函数的多返回值
{x, y} = _getCoords(encoded)

# 结构体字面量初始化（按字段顺序）
point: Point = {10, 20}
```

---

## 9. 注释

使用 `#` 进行行注释：

```python
# 这是一整行注释
count: int = 0    # 这是行尾注释
```

目前不支持多行注释，较长的注释需每行都加 `#`。

---

## 下一步

- [如何部署与调用合约](./how-to-deploy-and-call-a-contract.md) — 编译输出与链上调用流程

---

[🇬🇧 English version](../en/how-to-write-a-contract.md)
