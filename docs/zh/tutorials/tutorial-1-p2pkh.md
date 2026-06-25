# 教程一：P2PKH 合约入门

---

`P2PKH`（Pay to Public Key Hash）是最经典、最常见的 UTXO 锁定模式。这个教程不再用 “永真 Hello World” 起步，而是直接从真实可用的签名合约开始。

完成本教程后，你将能够：

- 理解 `P2PKH` 的锁定条件与解锁参数
- 写出并编译一份完整 `p2pkh` 合约
- 掌握 `Clone` 在签名验证里的必要性
- 用调试器验证通过/失败两条执行路径

---

## P2PKH 要解决什么问题

目标很简单：只有拥有某个私钥的人，才能花费这个 UTXO。

链上验证过程通常分两步：

1. 公钥哈希匹配：`Hash160(pubKey) == pubKeyHash`
2. 签名有效：`CheckSig(sig, pubKey) == true`

其中 `pubKeyHash` 是合约部署时固定写入的值（通常是收款人的地址哈希）。

---

## 第一步：编写 p2pkh 合约

创建文件 `p2pkh.ct`：

```python
Contract P2PKH:
    def verify(sig: hex, pubKey: hex):
        pubKey_copy = pubKey.Clone()
        pubKeyHash = Hash160(pubKey_copy)
        EqualVerify(pubKeyHash, self.pubKeyHash)
        Return CheckSig(sig, pubKey)
```

这段代码对应了标准 `P2PKH` 逻辑：

- `verify(sig, pubKey)`：解锁时提交签名和公钥
- `self.pubKeyHash`：合约中保存的目标公钥哈希
- `EqualVerify(...)`：哈希不匹配就立即终止
- `CheckSig(...)`：验证签名与公钥是否匹配
- `Return`：把签名验证结果作为最终脚本结果

---

## 可选：使用标准库模板

仓库自带了 `stdlib/std/p2pkh.ct`，可以直接复用标准 P2PKH 模板：

```python
import std.p2pkh

Contract P2PKH:
    def verify(sig: hex, pubKey: hex):
        ok = verifyP2PKH(sig, pubKey)
        Return ok
```

`verifyP2PKH` 内部同样会比较 `Hash160(pubKey)` 与 `self.pubKeyHash`，再执行 `CheckSig`。这种写法适合在多个合约中共享相同签名验证逻辑。

---

## 第二步：为什么必须 Clone

关键行是：

```python
pubKeyCopy = pubKey.Clone()
```

原因：`Hash160(pubKeyCopy)` 会消耗其输入值，而后面 `CheckSig(sig, pubKey)` 还需要原始 `pubKey`。  
如果不先 `Clone`，编译器会报“变量已被消耗”。

经验法则：

- 同一个变量要被多个操作使用时，在第一次消耗前 `Clone`
- 最后一次使用可以直接消耗原变量

---

## 第三步：编译合约

```bash
./utxo_interpreter p2pkh.ct
```

编译成功后会输出 JSON 结果，其中应包含：

- 合约名 `P2PKH`
- 公有函数 `verify(sig: hex, pubKey: hex)`
- `lock.hex` 和 `lock.asm`
- `abi` / `functions` 参数信息

默认输出文件为当前目录下的 `p2pkh.json`。

---

## 第四步：进入调试器验证

```bash
./utxo_interpreter p2pkh.ct --debug
```

运行后输入参数（示例）：

```
Enter parameters for verify:
  sig    [hex]: 0x3045022100...
  pubKey [hex]: 0x03ab12...
```

### 验证通过路径

使用与 `pubKeyHash` 匹配的公钥，并提供对应签名：

- `EqualVerify` 通过
- `CheckSig` 返回 1
- 合约最终返回真

### 验证失败路径

你可以故意输入错误参数测试拒绝逻辑：

- 公钥不匹配：在 `EqualVerify` 处直接终止
- 签名不匹配：`CheckSig` 返回 0，最终失败

---

## 常见问题

### 1) `pubKeyHash` 用什么格式？

使用 `hex`，通常是 20 字节（`Hash160` 结果）。

### 2) 为什么不用 `Equal` 而用 `EqualVerify`？

`EqualVerify` 在失败时立即终止，更符合“必须满足”的约束语义，代码也更简洁。

### 3) P2PKH 和地址是什么关系？

钱包地址通常是对 `pubKeyHash` 的编码表示。合约里真正比较的是哈希值本身，而不是地址字符串。

---

## 小结

你已经完成了第一份真实可用的 UTXO 合约，并掌握了 `P2PKH` 的核心：

- 解锁函数 `verify` 验证身份（哈希 + 签名）
- `Clone` 解决所有权/消耗问题
- 调试器可验证成功与失败两条路径

---

## 下一步

- [教程二：Counter 合约](./tutorial-2-counter.md) — 学习如何设计带状态推进逻辑的合约

---

[🇬🇧 English version](../../en/tutorials/tutorial-1-p2pkh.md)
