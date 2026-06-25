# 比特币基础

---

本节介绍编写 UTXO 合约所必须掌握的比特币底层概念，包括 UTXO 模型的工作方式、Bitcoin Script 的执行机制，以及 UTXO_Compiler 在其中扮演的角色。已熟悉这些内容的读者可以直接跳到 [如何编写合约](./how-to-write-a-contract.md)。

---

## 比特币交易结构

一笔比特币交易包含若干**输入（inputs）**和**输出（outputs）**。

### 输出（Output）

每个输出包含两部分：

- **value**：这个输出锁定了多少聪（satoshi，1 BTC = 10⁸ satoshis）
- **locking script（锁定脚本）**：一段字节码，描述"谁、在什么条件下可以花费这笔钱"

输出一旦创建，它的 value 和 locking script 就固定下来，直到有人花费它为止。未被花费的输出称为 **UTXO（Unspent Transaction Output）**。

### 输入（Input）

每个输入包含：

- **对前序输出的引用**：上一笔交易的 txid + 该交易中输出的索引号（vout）
- **unlocking script（解锁脚本）**：提供"钥匙"——满足锁定脚本设定条件所需的数据

### 执行模型

当矿工验证一笔交易时，对每个输入，它会将解锁脚本和对应的锁定脚本**拼接后依次执行**。最终主栈顶端的值为真（非零），才说明这个输入有效，花费被允许。

```
执行顺序：
[解锁脚本操作码序列]  →  [锁定脚本操作码序列]
         ↓
       BVM 主栈
         ↓
     栈顶 = true ?  →  ✓ 花费有效
     栈顶 = false?  →  ✗ 花费被拒绝
```

---

## BVM 栈机制

BVM（Bitcoin Virtual Machine）是一台**基于栈**的状态机，没有寄存器，没有堆，只有两个栈：

- **主栈（main stack）**：大多数操作在主栈上进行
- **副栈（alt stack）**：临时存储区，数据通过 `OP_TOALTSTACK` / `OP_FROMALTSTACK` 与主栈交换

每条操作码要么从栈顶弹出若干值作为输入，要么将结果压入栈顶，或两者兼有。

### 一个简单例子

执行 `<pubKeyHash> OP_EQUAL`：

```
初始栈（由解锁脚本压入）：      [computedHash]
执行 OP_EQUAL：             弹出 computedHash 和 pubKeyHash，压入比较结果
最终栈：                    [1]   ← true，验证通过
```

### 为什么所有权与栈模型有关

在 BVM 里，每个栈上的值只有一份。操作码 `OP_DUP` 负责复制栈顶值（对应合约语言里的 `.Clone()`），而大多数操作码执行后会**消耗**（弹出）它们的操作数。

这直接解释了合约语言的所有权规则：变量对应栈上的值，传给函数 = 弹出，只能用一次；`.Clone()` = `OP_DUP`，先复制再用。

---

## 锁定脚本的分类

### P2PKH（Pay to Public Key Hash）

最常见的比特币支付形式。锁定脚本格式：

```
OP_DUP OP_HASH160 <pubKeyHash> OP_EQUALVERIFY OP_CHECKSIG
```

验证逻辑：
1. 复制解锁方提供的公钥
2. 计算公钥的 Hash160
3. 与预存的 pubKeyHash 比较，必须相等
4. 用公钥验证签名

对应的 UTXO_Compiler 合约：

```python
Contract P2PKH:
    # Pay-to-Public-Key-Hash 验证合约
    # 验证交易签名是否与公钥哈希匹配

    def verify(sig: hex, pubKey: hex):
        # 验证函数：检查签名和公钥
        # 参数: sig: 交易签名  pubKey: 公钥
        pubKey_copy = pubKey.Clone()
        
        # 计算公钥的哈希值
        pubKeyHash = Hash160(pubKey_copy)
        
        # 验证公钥哈希是否匹配
        # 使用成员变量 self.pubKeyHash
        EqualVerify(pubKeyHash, self.pubKeyHash)
        
        # 验证签名
        result = CheckSig(sig, pubKey)
```

### P2SH（Pay to Script Hash）

锁定脚本只存储脚本的哈希。花费时，解锁方提供原始脚本和对应的解锁数据。UTXO_Compiler 目前不直接生成 P2SH 包装，但编译输出的字节码可作为 redeem script 使用。

### 自定义合约脚本

这正是 UTXO_Compiler 的核心应用场景：编写具有任意自定义逻辑的锁定脚本，实现多签、时间锁、哈希锁、抵押借贷等链上协议。

---

## 合约状态在 UTXO 模型中如何存在

以太坊合约有"链上存储"——一张键值表，合约函数可以读写它。UTXO 模型里**没有**这样的机制。那合约的状态存放在哪里？

答案是：**状态编译进字节码本身**，随着 UTXO 转移。

在 UTXO_Compiler 中，合约成员变量被直接嵌入编译出的字节码。用户提供不同的数据，产生不同的锁定脚本，也就是不同的"合约实例"。

```
合约源码 (模板)
     │
     │ 实例化（用户提供数据）
     ▼
锁定脚本字节码（含固定状态值）
     │
     │ 嵌入交易输出
     ▼
   UTXO（链上存在）
```

如果合约需要在执行后更新状态（例如借贷合约结清后转移抵押物），新的状态通过生成一个**新的 UTXO**来体现——新 UTXO 的锁定脚本包含更新后的状态字节码。这种"状态随资产转移"的模式，是 UTXO 合约设计的精髓，也是 `SuffixData` 机制的由来。

---

## 交易验证与 pretx 参数

UTXO_Compiler 合约经常需要验证**父交易**（花费当前 UTXO 的那笔交易的输入来源交易），以防止解锁方伪造参数。

BVM 执行时提供了 `BVM.unlockingInput`（当前输入的原始数据，含 txid 引用），合约可以：

1. 接收解锁方传入的父交易数据（`pretx` 参数）
2. 手动计算父交易的 txid
3. 与 `BVM.unlockingInput` 中记录的 txid 对比
4. 一致则说明父交易数据是真实的

这个验证模式在 [进阶案例](./tutorials/tutorial-2-counter.md) 中有详细演示。

---

## 小结

| 概念 | 含义 | 对应合约语言 |
|------|------|------------|
| UTXO | 未花费的交易输出，携带 value 和锁定脚本 | 一个 `.ct` 合约实例 |
| 锁定脚本 | 花费条件的字节码描述 | 编译输出 |
| 解锁脚本 | 花费时提供的数据和证明 | 公有函数的调用参数 |
| BVM | 执行脚本的栈式虚拟机 | 所有权系统的底层依据 |
| 主栈 | BVM 主工作区 | 局部变量 |
| 副栈 | BVM 临时存储区 | `SetAlt` / `SetMain` |
| OP_DUP | 复制栈顶值 | `.Clone()` |

---

## 下一步

- [如何编写合约](./how-to-write-a-contract.md) — 学习合约语言的完整语法

---

[🇬🇧 English version](../en/bitcoin-basics.md)
