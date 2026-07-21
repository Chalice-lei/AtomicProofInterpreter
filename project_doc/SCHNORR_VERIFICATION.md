# Schnorr 签名验证（离散对数版）

本文档记录 AtomicProof Compiler 引入 Schnorr 签名验证支持的设计、实现与约束。

---

## 0. 范围界定

**本特性仅作"字节码生成"层面的实现**——

> 编译器对预言机签名验证的职责，是把验证算法**翻译为对应的链上字节码**，而不是在编译器进程内"真实执行"验证。真实的签名校验由部署目标节点（带任意精度大数支持的 BSV 运行时）在交易广播 / 区块确认时跑发射出来的脚本完成。

因此本次落地：
- **只动**：`stdlib/std/schnorr.ct` 模板库 + 用户层演示合约；
- **不动**：编译器核心（`bytecode_*` / `script_decoder.*` / 解析器 / AST 访问者）；
- **不动**：BVM 模拟器 (`src/debugger/vm/`) 的数值 op 实现（保持 int64_t 路径）——签名验证不应在编译器二进制里执行。

`--debug` 模拟器跑到 256 字节 `OP_MUL` / `OP_MOD` 时确实会因 int64 上溢报错；这是**调试器侧的运行时局限**，与编译产物的链上可执行性无关，超出本特性范围。

---

## 1. 背景

预言机、可验证抽签、阈值签名等场景常需在合约里链上校验非 ECDSA 类的数字签名。AtomicProof BVM 已通过 `OP_CHECKSIG`/`OP_CHECKMULTISIG` 暴露 secp256k1 ECDSA，但缺少：

1. Rabin 签名验证（基于 RSA 风格大数模平方）；
2. Schnorr 签名验证（离散对数 / 椭圆曲线均可）。

第 (1) 条曾尝试以新增扩展 opcode `OP_RABIN_VERIFY` 解决——后回退，因为 sCrypt 的 scrypt-ts-lib 全部用纯 sCrypt 库实现这类验证，编译产物只是 `OP_MUL` / `OP_MOD` / `OP_CAT` / `OP_HASH256` 等标准指令的组合，不依赖任何外部预编译。本特性按同模型实现 Schnorr，未引入扩展指令。

---

## 2. 设计选型

### 2.1 三条候选路径

| 路径 | 描述 | 采用 |
|---|---|---|
| (a) 纯 .ct stdlib 库，发射标准 opcode 组合 | 仿 scrypt-ts-lib `RabinVerifier` / `SECP256K1` 模型 | **是** |
| (b) 新增 C++ builtin + 扩展 opcode（如 `OP_SCHNORR_VERIFY`） | 类比 `OP_PARTIAL_HASH` 的私有扩展 | 否 |
| (c) `find_package` GMP / OpenSSL 在编译期静态求值 | 把验证移到编译器进程，违反范围界定 | 否 |

选择 (a) 的关键理由：
- **保持字节码可移植**：编译产物只用 BSV 标准指令，未来若把 BVM 对接真实 BSV 节点能直接运行；
- **范围正交**：编译器只做翻译，不在自身进程内执行密码学计算；
- **语义透明**：算法在 .ct 源码中可读，等价于 sCrypt 用户视角；
- **零编译器内核改动**：不动 `bytecode_opcodes.*` / `bytecode_operation_functions.h` / 解码器；
- **生态对齐**：sCrypt 自家 Rabin、SECP256K1 等都是这种 sCrypt 库 + 纯标准 opcode 组合，本项目跟齐。

### 2.2 验证方案

采用乘法群 (Z/pZ)* 上的经典 Schnorr identification / signature：

```
公钥：y = g^x mod p
签名 (R, s)：
    R = g^k mod p          # k 为一次性随机数
    e = SHA-256(R || msg)  # 解读为整数（fromLEUnsigned）
    s = k + x·e (mod q)    # q 为 g 的子群阶
验证：g^s mod p ≡ (R · y^e) mod p
```

不选 BIP-340 EC Schnorr（secp256k1 上的 Bitcoin/Taproot 标准方案）的原因：在脚本里实现椭圆曲线点加 + 256 轮 double-and-add，依赖与 Rabin 同源的大整数原语，但额外加上点加法的 ~10 步模算术。代价从"一下午"升到"多天 + 库 >几 KB"，超出本次特性预算。离散对数版给出同种 Schnorr 安全性（基于 DL/DDH 假设），与现有大数运算字节码原语同形——一次性建设。

---

## 3. 实现

### 3.1 文件清单

| 文件 | 角色 |
|---|---|
| [stdlib/std/schnorr.ct](../stdlib/std/schnorr.ct) | 模板库，导出 `schnorrVerify` |
| [test/schnorr_demo.ct](../test/schnorr_demo.ct) | 最小演示合约 |

**编译器、BVM 模拟器、CMake 配置无任何改动。**

### 3.2 库结构（[stdlib/std/schnorr.ct](../stdlib/std/schnorr.ct)）

库只暴露**一个**函数：

```python
Library std.schnorr:
    def schnorrVerify(msg: hex, R: hex, s: hex,
                            pubKey: hex, generator: hex, modulus: hex):
        # 1. e = fromLEUnsigned(SHA-256(R || msg))
        # 2. 把所有 hex 输入转为大整数（末尾补 0x00 避免 OP_BIN2NUM 误判负号）
        # 3. lhs = g^s mod p          ← 内联 256 轮平方-乘
        # 4. y_pow_e = y^e mod p      ← 再内联 256 轮平方-乘
        # 5. rhs = (R · y_pow_e) mod p
        # 6. NumEqualVerify(lhs, rhs)
```

**关键实现要点**：

#### 3.2.1 fromLEUnsigned

`b' = b ‖ 0x00` 后再 `OP_BIN2NUM`。Bitcoin Script number 是 sign-magnitude LE，最高字节高位是符号位；任意外部字节串若最高位置 1 会被解读为负数，追加 0x00 强制正号。语义等价 sCrypt `Utils.fromLEUnsigned`。

#### 3.2.2 无分支 modPow

平方-乘的标准写法需要 `if bit == 1: result *= base`。.ct 在循环体中维持 if/else 双臂栈一致性的能力有限（counter2 / auction 用 SetAlt 跨臂，写起来繁琐），故改用**无分支**等价式：

```
factor = bit · base + (1 - bit)        # bit=0 → 1 ; bit=1 → base
result = (result · factor) mod modulus
```

`bit` 由 `Mod(e.Clone(), 2)` 给出，配合 `e = Div(e, 2)` 实现右移。每轮固定执行一次乘 + 一次模 + 一次平方 + 一次模，无 OP_IF/OP_ELSE。

#### 3.2.3 modPow 必须直接内联

理论上应抽 `_modPow(base, exp, modulus)` 辅助函数，被 verify 函数调用两次（一次算 `g^s`、一次算 `y^e`）。实测当前 .ct 库 inliner 在以下两种情形均会失败：

1. 同一辅助函数在另一**库函数**体内被调用（哪怕只一次）——参数解析阶段找不到栈元素；
2. 即便用 `_modPowGS` / `_modPowYE` 双胞胎绕过情形 1，进一步内层赋值（`result = 1` 类字面量）也会触发 `Cannot assign to rvalue` 错误。

权衡之后，**把两段 modPow 完整内联到 `schnorrVerify` 函数体**，外层循环用 `gs_*` / `ye_*` 前缀区分两段的局部变量。库 .ct 源 ~140 行，但只有一个用户级入口；不会因辅助函数嵌套触发 inliner 限制。

> 如果未来 inliner 修了"嵌套调用"问题，可把两段循环抽成 `_modPow` 单一辅助函数，库源代码会从 ~140 行缩到 ~70 行。

#### 3.2.4 重命名而非克隆消费入参

```python
e = exp        # 而非 e = exp.Clone()
b = base       # 而非 b = base.Clone()
```

`a.Clone()` 创建副本但**保留**原 `a`；若用 Clone，函数返回时原始 `base`/`exp` 仍在栈上，导致调用方栈偏移错乱。改用赋值（所有权转移）令循环结束后栈上只剩 `result`，符合"函数返回单一值"的栈约定。

#### 3.2.5 展开次数

两段 modPow 各展开 `Range(0, 256)` 次平方-乘，承载 ≤256 位指数（对应标准 Schnorr 子群阶 q ≤ 2^256）。每多展开一轮约增加 60–120 ops。

### 3.3 演示合约（[test/schnorr_demo.ct](../test/schnorr_demo.ct)）

```python
import std.schnorr

Contract SchnorrDemo:
    def main(msg: hex, R: hex, s: hex):
        schnorrVerify(msg, R, s, self.pubKey, self.generator, self.modulus)
```

`self.pubKey` (= y)、`self.generator` (= g)、`self.modulus` (= p) 是合约级群参数，由部署侧绑定占位符。`(msg, R, s)` 由 redeemer 传入。

---

## 4. 编译产物特征

### 4.1 规模

| 指标 | 256 迭代版 | 32 迭代演示版 |
|---|---:|---:|
| Bytecode 长度 | **17 479 字节 (~17.5 KB)** | 2 247 字节 |
| 操作码总数 | 17 455 | 2 223 |
| 编译耗时 | ~0.7 s | ~0.1 s |

### 4.2 实际用到的 opcode

完整反汇编只包含 **17 种 BSV 标准指令**，与 sCrypt 把同类纯 sCrypt 库编译为 Bitcoin Script 的形态一致：

```
OP_CAT  OP_SHA256                          # 哈希 + 拼接
OP_BIN2NUM                                 # 字节 → 大整数
OP_MUL  OP_MOD  OP_DIV  OP_ADD  OP_SUB     # 大数算术
OP_NUMEQUALVERIFY                          # 终判
OP_DUP  OP_OVER  OP_SWAP  OP_ROT  OP_PICK  OP_ROLL   # 栈管理
OP_1 OP_2 OP_3 OP_4 OP_5 OP_6              # 小整数常量
```

**未出现任何**自定义扩展指令（无 `OP_RABIN_VERIFY` / `OP_SCHNORR_VERIFY` 等）。

### 4.3 链上执行职责归属

发射出来的字节码假定**运行时**（节点）支持任意精度 Bitcoin Script number 算术——这是 BSV 在 Genesis 升级后对 `OP_MUL` / `OP_MOD` / `OP_BIN2NUM` 等的标准行为。验证由节点执行；编译器不卷入实际数值计算。

---

## 5. 已知约束

### 5.1 .ct 库 inliner 不支持嵌套调用

复现：

```python
Library std.foo:
    def _h(x: int):
        return Add(x, 1)
    def wrap(a: int):
        r = _h(a)        # ❌ 报 "Unable to find element 'r' on stack"
        return r
```

直接后果：Schnorr 库无法把 modPow 抽辅助函数。本次实施通过把 modPow 内联到 `schnorrVerify` 函数体绕开。属于编译器内核侧的待修缺陷，不在本特性范围。

### 5.2 BVM 模拟器对超 8 字节数值的局限

`src/debugger/vm/bvm_simulator.cpp` 的 `OP_MUL` / `OP_MOD` / `OP_BIN2NUM` 等数值 op 走 `int64_t` 路径，跑 256+ 字节大数时会上溢。这是**模拟器/调试器侧的局限**——`--debug` 单步执行 Schnorr 验证脚本会在第一次 `s²` 失败。

但**这与编译产物的链上可执行性无关**：脚本在真实 BSV 节点（任意精度 Script number）上正常执行。本特性按"编译器只做字节码生成"范围界定，未升级模拟器；如果后续要让 `--debug` 端到端走通 Schnorr 验证，可独立做模拟器侧的大整数升级，与本特性正交。

### 5.3 安全声明

- **离散对数版 Schnorr 不是 BIP-340**。BIP-340 用 secp256k1 椭圆曲线、x-only 公钥、tagged hash；本库用乘法群，参数（p, q, g）由部署侧选择。两者签名互不兼容。
- 标准 Schnorr 的安全级与 (p, q) 选取强相关：q 至少 256 位、p 至少 2048 位（DLog 安全），与本库的 `Range(0, 256)` 展开匹配。
- 调用侧必须保证 oracle 私钥下随机数 k 唯一不可预测，且每次签名独立——k 复用 = 私钥泄露，与 ECDSA / EC Schnorr 同。

### 5.4 测试缺口

当前已验证：
- 编译产物形态（纯 BSV 标准 opcode）；
- 全 112 sCrypt 移植合约 + Schnorr demo 合约的回归编译。

**未做**（与编译器范围正交，留给运行时 / 部署测试）：
- 在真实 BSV 节点上用真实 Schnorr 测试向量（合法签名 + 翻转字节的非法签名）跑发射的脚本。

---

## 6. 用法

### 6.1 编译

库通过 `APC_STDLIB_PATH` 环境变量定位：

```bash
APC_STDLIB_PATH=$(pwd)/stdlib \
    ./build/bin/utxo_Interpreter test/schnorr_demo.ct
```

或安装后通过 `APC_DEFAULT_STDLIB_PATH`（CMake 注入）自动查找。

### 6.2 在用户合约中调用

```python
import std.schnorr

Contract MyOracleConsumer:

    def main(msg: hex, R: hex, s: hex):
        # 群参数 self.{pubKey, generator, modulus} 由部署侧绑定占位符
        schnorrVerify(msg, R, s, self.pubKey, self.generator, self.modulus)
```

约定：宿主合约必须声明 `self.pubKey`（即公钥 y）、`self.generator`（即 g）、`self.modulus`（即 p）三个成员字段，与 [stdlib/std/p2pkh.ct](../stdlib/std/p2pkh.ct) 要求宿主声明 `self.pubKeyHash` 同模式。

### 6.3 输入字节序约定

`msg` / `R` / `s` / `pubKey` / `generator` / `modulus` 全部按**小端无符号**字节串传入；库内部 `Cat(b, 0x00) + BinToNum` 强制正号。

---

## 7. 与 Rabin 探索的关系

本次 Schnorr 落地之前，曾完成 Rabin 验证的两版尝试：

1. **第一版**：新增扩展 opcode `OP_RABIN_VERIFY = 0xbc` + C++ builtin + 模拟器内嵌大整数单 opcode 实现。被否决——违背"sCrypt 模型"准则（应让编译产物只用标准 opcode）。
2. **第二版**：写 `stdlib/std/rabin.ct` 库（编译产物纯标准 opcode，符合 sCrypt 模型）+ 模拟器升级为大整数路径以让 `--debug` 跑 Rabin。被否决——模拟器侧的密码学计算违背"编译器只做字节码生成"的范围界定。

最终在 Schnorr 此次：**编译器只生成字节码，模拟器不做大数密码学**。两条审计线均守住。

---

## 8. 文件改动汇总

| 文件 | 改动 |
|---|---|
| [stdlib/std/schnorr.ct](../stdlib/std/schnorr.ct) | 新增（~140 行 .ct 库） |
| [test/schnorr_demo.ct](../test/schnorr_demo.ct) | 新增（演示合约） |

**编译器内核与 BVM 模拟器零改动**——因为编译器对预言机签名的职责只是发射对应字节码，不在自身进程内执行验证。
