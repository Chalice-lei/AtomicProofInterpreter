# 合约级全局常量回归

本目录验证 `global Name = <scalar literal>` 在 Interpreter 的公共前端和
bytecode 管线中具有一致语义：

- 引用被直接替换为字面量，不进入 ABI、运行时栈或调试变量；
- Library 函数可以读取导入合约的全局常量；
- 重复、遮蔽、赋值、位置错误和非字面量初始值会在前端拒绝；
- `compile` / `ast` / `run` / `shell` / `compiler-repl` 共用相同的解析结果。

在仓库根目录运行：

```bash
bash test/global_constants/run_global_constant_regression.sh \
  build/bin/utxo_Interpreter
```

`run` 和 REPL 验证仅在可执行文件带 debugger/VM 支持时运行。
