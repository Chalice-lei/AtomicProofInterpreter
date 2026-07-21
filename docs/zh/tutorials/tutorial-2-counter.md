# 教程二：Counter 合约实战

---

本教程改用真实的 `counter.ct` 合约。它不是简单的“`count + 1` 比较”，而是一个**交易级状态校验**示例：  
先从父交易中提取旧计数，再验证当前交易输出中的新计数必须递增。

完成本教程后，你将掌握：

- 如何理解 `Struct` 驱动的交易数据建模
- 如何从 `PreTX` 提取旧状态并做父交易一致性校验
- 如何在 `CurrentTX` 中验证计数必须递增
- 如何读懂 `SetMain/SetAlt/Push(BVM.*)` 这类底层栈操作

---

## 合约目标

`counter.ct` 的核心目标是保证：

- 当前被花费输入确实来自声明的父交易（防止伪造前置状态）
- 新交易输出中对应脚本的计数值，必须等于旧计数 + 1
- 当前交易输出哈希必须与链上 `BVM.outputsHash` 一致

---

## 第一步：理解合约结构

```python
Contract Counter:
    Struct Script:
        SuffixData: string
        PartialHash: string
        Size: number

    Struct Output:
        Value: number
        LockingScript: Script

    Struct Input:
        Data: {txid: hex32, vout: hex4, sequence: hex4}

    Struct PreTX:
        VLIO: string
        Inputs: Input[3]
        UnlockingScriptHash: string
        Outputs: Output[3]

    Struct CurrentTX:
        Outputs: Output[3]

    def getCountFromPreTX(pretx: PreTX):
        utxoData: {txid: hex32, vout: hex4, sequence: hex4}
        utxoData = Push(BVM.unlockingInput)
        vout = BinToNum(utxoData.vout)
        vout_copy = vout.Clone()
        Delete(utxoData.sequence)
        Delete(utxoData.txid)

        code_data = pretx.Outputs[vout_copy].LockingScript.SuffixData.Clone()
        pre_count = BinToNum(code_data.Slice(1, 8))
        SetAlt(pre_count)

        vout_copy = vout.Clone()
        pre_code_partialhash = pretx.Outputs[vout_copy].LockingScript.PartialHash.Clone()
        SetAlt(pre_code_partialhash)
        pre_code_size = pretx.Outputs[vout_copy].LockingScript.Size.Clone()
        SetAlt(pre_code_size)

        #验证父交易
        #拼接输出
        tx_data = Push(0)
        SetAlt(tx_data)
        for i in Range(2, -1, -1):
            size_copy = pretx.Outputs[i].LockingScript.Size.Clone()
            if size_copy != 0:
                tx_data_temp = PartialHash(pretx.Outputs[i].LockingScript.SuffixData, pretx.Outputs[i].LockingScript.PartialHash, pretx.Outputs[i].LockingScript.Size)
                tx_data_temp = Cat(pretx.Outputs[i].Value, tx_data_temp)
                SetMain(tx_data)
                tx_data = Cat(tx_data_temp, tx_data)
                SetAlt(tx_data)
                # Keep(tx_data)
            else:
                Delete(pretx.Outputs[i].LockingScript.Size)
                Delete(pretx.Outputs[i].LockingScript.PartialHash)
                Delete(pretx.Outputs[i].LockingScript.SuffixData)
                Delete(pretx.Outputs[i].Value)

        #开始计算txid
        SetMain(tx_data)
        tx_data = Sha256(tx_data)
        tx_data = Cat(pretx.UnlockingScriptHash, tx_data)
        SetAlt(tx_data)
        #拼接输入
        tx_input_data = Push(0)
        for i in Range(2, -1, -1):
            tx_input_data = Cat(pretx.Inputs[i].Data, tx_input_data)
        tx_input_hash = Sha256(tx_input_data)
        SetMain(tx_data)
        tx_data = Cat(tx_input_hash, tx_data)
        tx_data = Cat(pretx.VLIO, tx_data)
        #tx数据拼接完成，计算txid
        txid = Hash256(tx_data)
        preTXID = BVM.unlockingInput.Slice(0, 32) #获取当前解锁的输入txid
        EqualVerify(txid, preTXID)

    def verifyCurrentTX(ctx: CurrentTX):
        time = Push(3)
        SetAlt(time)
        outputs_data = Push(0)
        SetAlt(outputs_data)
        for i in Range(2, -1, -1):
            size = ctx.Outputs[i].LockingScript.Size.Clone()
            size_copy = size.Clone()
            if size_copy != 0:
                SetMain(outputs_data)
                SetMain(time)
                SetMain(pre_code_size)
                pre_code_size_copy = pre_code_size.Clone()
                if size == pre_code_size_copy:
                    SetMain(pre_code_partialhash)
                    pre_code_partialhash_copy = pre_code_partialhash.Clone()
                    code_partialhash = ctx.Outputs[i].LockingScript.PartialHash.Clone()
                    EqualVerify(pre_code_partialhash_copy, code_partialhash)
                    code_suffixdata = ctx.Outputs[i].LockingScript.SuffixData.Clone()
                    ctx_count = BinToNum(code_suffixdata.Slice(1, 8))
                    SetMain(pre_count)
                    pre_count_copy = pre_count.Clone()
                    EqualVerify(pre_count_copy + 1, ctx_count)
                    SetAlt(pre_count)
                    SetAlt(pre_code_partialhash)
                    time = time - 1
                    SetAlt(pre_code_size)
                    SetAlt(time)
                else:
                    SetAlt(pre_code_size)
                    SetAlt(time)
                SetAlt(outputs_data)
                outputs_data_temp = PartialHash(ctx.Outputs[i].LockingScript.SuffixData, ctx.Outputs[i].LockingScript.PartialHash, ctx.Outputs[i].LockingScript.Size)
                outputs_data_temp = Cat(ctx.Outputs[i].Value, outputs_data_temp)
                SetMain(outputs_data)
                outputs_data = Cat(outputs_data_temp, outputs_data)
                SetAlt(outputs_data)
            else:
                Delete(size)
                Delete(ctx.Outputs[i].LockingScript.Size)
                Delete(ctx.Outputs[i].LockingScript.PartialHash)
                Delete(ctx.Outputs[i].LockingScript.SuffixData)
                Delete(ctx.Outputs[i].Value)
        SetMain(outputs_data)
        outputs_data = Sha256(outputs_data)
        EqualVerify(outputs_data, BVM.outputsHash)
        SetMain(time)
        Return (time < 3 == 1)
        Push(self.count)
```

你可以把它理解成两阶段流程：

1. `getCountFromPreTX(pretx)`：从父交易取旧状态并校验父交易合法
2. `verifyCurrentTX(ctx)`：验证当前交易输出状态是否正确推进

---

## 第二步：关键函数解析

### `getCountFromPreTX(pretx)`

这个函数做三件事：

- 通过 `Push(BVM.unlockingInput)` 取得当前输入中的 `vout`，定位父交易被花费的输出
- 从该输出脚本的 `SuffixData` 中切片出旧计数 `pre_count`
- 重新拼接并哈希父交易数据，最后 `EqualVerify(txid, preTXID)` 校验父交易一致性

这一步的意义是：确保“旧计数”来源可信，不是调用方随便传入的假数据。

### `verifyCurrentTX(ctx)`

这个函数遍历当前交易输出并做状态推进验证：

- 对匹配脚本模板的输出，提取其 `ctx_count`
- 验证 `ctx_count == pre_count + 1`
- 汇总输出数据并计算 `Sha256(outputs_data)`，再与 `BVM.outputsHash` 比较
- 最终用 `Return (time < 3 == 1)` 确认至少有一个有效推进分支

---

## 第三步：编译

```bash
./utxo_Interpreter contract_file/counter.ct
```

预期可看到 `Counter` 合约及其公开校验函数（具体导出函数名以编译输出为准）。

---

## 第四步：调试验证建议

```bash
./utxo_Interpreter contract_file/counter.ct --debug
```

### 场景 A：正确递增

构造一组 `PreTX + CurrentTX`，让当前输出中的计数恰好为旧计数 + 1：

- `getCountFromPreTX` 通过
- `verifyCurrentTX` 中的递增约束通过
- 输出哈希匹配 `BVM.outputsHash`

### 场景 B：计数跳变

若当前计数不是旧值 + 1（例如直接 +3）：

- 在 `EqualVerify(pre_count_copy + 1, ctx_count)` 处失败
- 合约拒绝该交易

### 场景 C：父交易伪造

若传入的 `PreTX` 不能重建出正确 txid：

- 在 `EqualVerify(txid, preTXID)` 处失败
- 合约拒绝该交易

---

## 扩展建议

在这个版本基础上你可以继续扩展：

- 将 `Outputs[3]` / `Inputs[3]` 泛化为可配置长度
- 给计数推进增加上限、窗口期或权限签名
- 抽取交易拼接逻辑为复用函数，降低重复 `Cat/Sha256` 代码

---

## 小结

通过这个 `Counter` 实例，你已经掌握了“交易级状态约束”的核心写法：

- 用 `Struct` 描述输入/输出交易数据
- 先验证父状态，再验证当前状态推进
- 通过 `EqualVerify` 将关键约束做成强校验点
- 用调试器分别覆盖成功路径与拒绝路径

---

## 下一步

- [所有权系统](../advanced/ownership-system.md) — 深入理解变量消耗与 Clone 规则

---

[🇬🇧 English version](../../en/tutorials/tutorial-2-counter.md)
