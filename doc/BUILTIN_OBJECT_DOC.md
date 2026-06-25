# 内置对象文档

本文档基于详细介绍了系统定义的内置对象，包括Self对象和BVM对象。文档涵盖对象的名称、作用、属性、方法及使用示例，旨在为开发人员提供清晰、准确的函数使用指南。

## 1. Self 对象
- **对象名（Object Name）**：Self
- **描述（Short Description）**：表示“自身”概念的内置对象，用于处理与当前上下文相关的成员访问。
- **使用示例（Example）**：
    ```python
    # 使用Self对象访问合约成员变量
    Contract P2PKH:
        def verify(signature: hex, pubKey: hex):
            pubKey_copy = pubKey.Clone()
            pubKeyHash = Hash160(pubKey_copy)
            EqualVerify(pubKeyHash, self.pubKeyHash)
            result = CheckSig(signature, pubKey)
    ```

## 2. BVM 对象
- **对象名（Object Name）**：BVM（Bytecode Virtual Machine，字节码虚拟机）
- **描述（Short Description）**：表示字节码虚拟机的内置对象，提供虚拟机元数据（如版本、输入输出计数）的访问能力。
- **预定义成员映射表**：
    | 成员名 | 对应数值 | 用途 |
    |--------|----------|------------|
    | "version" | 1 | 虚拟机版本号 |
    | "locktime" | 2 | 锁定时间 |
    | "inputCount" | 3 | 输入数据数量 |
    | "outputCount" | 4 | 输出数据数量 |
    | "inputsHash" | 5 | 输入数据的哈希值 |
    | "unlockingInput" | 6 | 解锁输入数据 |
    | "outputsHash" | 7 | 输出数据的哈希值 |
- **使用示例（Example）**：
    ```python
    # 处理交易数据并验证
    tx_data = Cat(pretx.VLIO, tx_data)  # 拼接交易数据
    txid = Hash256(tx_data)  # 计算交易ID

    # 访问BVM对象的元数据
    meta_data = BVM.unlockingInput  # 获取解锁输入元数据
    {meta_data_txid, meta_data_remain} = Split(meta_data, 32)  # 从元数据中提取交易ID

    EqualVerify(txid, meta_data_txid)  # 验证交易ID一致性
    ```
