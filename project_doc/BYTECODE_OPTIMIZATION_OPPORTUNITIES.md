# 字节码生成优化机会分析

> 本文档对 AtomicProof Compiler（`apc` / `utxo_interpreter`）当前字节码生成路径做一次机会盘点，目的是为"进一步减少生成字节码量"提供可落地的改进清单。文档**不含基准数据**（待后续 benchmark 补充），也**不是任何单一 PR 的实施文档**，而是供团队/后续 Contributor 取用的参考。

---

## 一、背景与目标

当前编译 Pipeline 注册于 [main.cpp:1331-1343](../main.cpp#L1331-L1343)：
`LexerPass → ParserPass → ASTToBytecodePass（含 CollectSymbols / PreAnalysis / StaticInfo / ConstantFolder / AstToBytecode 五个子 Visitor）→ ExportResultsPass`。

经盘点，字节码生成路径上**已有相当多的点状优化**（见 §二）；但**缺少字节码层的窥孔 Pass**，也缺少**跨表达式/跨分支的活跃性与死代码分析**。本文围绕"减小最终脚本字节数"这个目标，整理三个梯度的改进机会。

## 二、现状盘点（已实现、请勿重复提案）

| 范畴 | 已有能力 | 位置 |
|------|---------|------|
| AST 层常量折叠 | 一元 `-` / `!`；二元 `+ - * / % == != < > <= >= && \|\|`（仅 Number） | [constant_folder.cpp:259-331](../src/compiler/constant_folder.cpp#L259-L331) |
| 单赋值常量传播 | 标量 + 整元素字面量数组；循环体内赋值自动失效；按函数隔离 | [constant_folder.cpp:23-38](../src/compiler/constant_folder.cpp#L23-L38)、[:342-384](../src/compiler/constant_folder.cpp#L342-L384) |
| 小整数编码 | `0 → OP_0`、`-1 → OP_1NEGATE`、`1..16 → OP_1..OP_16` | [bytecode_helper_fun.h:60-79](../src/bytecode/bytecode_helper_fun.h#L60-L79) |
| PushData 长度自适应 | ≤75 直接前缀、≤255 PUSHDATA1、≤65535 PUSHDATA2、否则 PUSHDATA4 | 同上 |
| Roll 微优化 | `ROLL(0) → ∅`、`ROLL(1) → OP_SWAP`、`ROLL(2) → OP_ROT` | [ast_to_bytecode_visitor.cpp:3442-3464](../src/compiler/ast_to_bytecode_visitor.cpp#L3442-L3464) |
| Pick 微优化 | `PICK(0) → OP_DUP`、`PICK(1) → OP_OVER` | [ast_to_bytecode_visitor.cpp:3466-3484](../src/compiler/ast_to_bytecode_visitor.cpp#L3466-L3484) |
| 二元运算预分析 | 标识符-标识符在栈上的四种位置形态（A/A'/B/B'）+ 可交换运算利用；`x±1 → OP_1ADD/OP_1SUB`；`x±0`、`x*1`、`x/1` 恒等式；`0-x → OP_NEGATE` | [ast_to_bytecode_visitor.cpp:1918-2110](../src/compiler/ast_to_bytecode_visitor.cpp#L1918-L2110) |
| if 条件反相合并 | `if (a != b)` → `OP_EQUAL + OP_NOTIF`（省 1 字节，免 OP_NOT） | [ast_to_bytecode_visitor.cpp:351-369](../src/compiler/ast_to_bytecode_visitor.cpp#L351-L369) |
| 作用域清理 | 按栈位排序后批量清理；连续 DROP 合并为 `OP_2DROP`；次栈顶用 `OP_NIP`（替 ROLL+DROP） | [ast_to_bytecode_visitor.cpp:233-294](../src/compiler/ast_to_bytecode_visitor.cpp#L233-L294) |
| 零成本赋值 | 新变量 = 栈上值时原地 `renameAtPosition`，不发射字节码 | [ast_to_bytecode_visitor.cpp:805-854](../src/compiler/ast_to_bytecode_visitor.cpp#L805-L854) |

## 三、优化机会

按"预期 ROI × 独立性"排序成三个 Tier。每条给出：**问题 → 位置 → 做法要点 → 风险/注意**。

### Tier 1 — 高 ROI、相对独立，建议优先立项

#### 3.1 字节码层 Peephole Optimizer（当前完全缺失）

- **问题**：AST 层优化只能看到"当前节点内部"的序列。跨节点拼接出来的冗余（如两个相邻语句在边界产生 `OP_SWAP OP_SWAP`、`OP_DUP 结合后的 OP_DROP`）没有任何 Pass 再扫一遍。
- **位置**：新 Pass，建议挂在 [bytecode_generator.cpp](../src/bytecode/bytecode_generator.cpp) 最终装配之后、[export_results_pass.h](../src/export_results_pass.h) 之前。
- **做法要点**：对线性指令流做滑动窗口（长度 2-3）匹配重写。典型规则（仅列未被覆盖的）：
  - `PUSH x ; OP_DROP` → ∅（干掉纯副作用表达式语句产生的栈顶）
  - `OP_SWAP ; OP_SWAP` → ∅
  - `OP_NOT ; OP_NOT` → `OP_0NOTEQUAL`
  - `OP_DUP ; OP_DROP` → ∅
  - 连续 `OP_DROP` 的更多合并（与作用域清理重叠但覆盖跨语句情形）
- **风险**：
  - 必须跳过受 `OP_IF/OP_ELSE/OP_ENDIF/OP_CODESEPARATOR` 分割的**控制流边界**（不同分支间的序列不能合并）。
  - 必须保留 `OP_SIZE` / `OP_DEPTH` 等依赖栈形的操作码的前序副作用。
  - 对 `--debug-output` JSON 里的 PC 映射要同步更新，否则断点会漂移。

#### 3.2 死分支 / 死循环消除

- **问题**：[ConstantFolder](../src/compiler/constant_folder.cpp) 已经会把 `if (1==1) {...}` 折叠成 `if (1) {...}`，但**条件折成字面量后没有进一步剪枝**，仍会生成 `OP_1 OP_IF ... OP_ELSE ... OP_ENDIF` 的完整控制流。
- **位置**：在 ConstantFolder 出口追加一个轻量 AST 重写；或独立为 `DeadBranchEliminator` Pass 放在 ConstantFolder 之后、AstToBytecodeVisitor 之前。
- **做法要点**：
  - `IfNode.condition` 为 Number 字面量 → 直接替换整条语句为对应分支的 `BlockNode`，丢弃另一侧。
  - `ForNode.iterable` 为空数组字面量 → 整条语句删除。
  - `require(literalTrue)` → 删除；`require(literalFalse)` → 编译期报错。
- **风险**：分支内如果有**有副作用的声明**（对外可见的变量），直接丢弃可能改变符号表；需沿用 ConstantFolder 里对循环体赋值 `+1` 计数的保守思路，只剪"整块纯计算"的死分支。

#### 3.3 ExprStmtNode 无副作用表达式立即 DROP

- **问题**：[ast_to_bytecode_visitor.cpp:916-921](../src/compiler/ast_to_bytecode_visitor.cpp#L916-L921) 的 `visit(ExprStmtNode&)` 只是 `visitExpr(...)`，表达式的结果若是运行时值（占位符），会留在栈顶直到作用域退出时被清理。间接导致后续每一次 ROLL 的距离都 +1。
- **位置**：同上入口。
- **做法要点**：
  - PreAnalysisVisitor 标注该表达式是否产生副作用（读链上数据、调用内置哈希等视情况判断）。
  - 无副作用且栈顶有值 → 在 ExprStmtNode 末尾 emit `OP_DROP`（后续 §3.1 的 peephole 还能与相邻序列进一步合并）。
- **风险**：当前很多表达式的"返回"其实是 placeholder，未必真正入栈，要配合 `m_scopePtr->top()` 检查。

---

### Tier 2 — 中等 ROI，多数是对已有优化的扩展

#### 3.4 常量折叠的运算覆盖扩展

- **问题**：[tryFoldBinary](../src/compiler/constant_folder.cpp#L259-L331) 仅覆盖算术 / 比较 / 逻辑运算；字面量 **位运算**（`& | ^ << >>`，若 `.ct` 语法支持）与**字符串/字节串拼接**（如 `"ab" + "cd"`、`hex"01" + hex"02"`）完全未参与折叠。
- **位置**：同文件 `tryFoldBinary` + 可能需扩展 `literalAsInt` 之外的字面量取值辅助。
- **做法要点**：
  - 先确认语法层面支持哪些字面量运算（查 [doc/GRAMMAR_SPECIFICATION.md](../doc/GRAMMAR_SPECIFICATION.md) 与 [src/parser/](../src/parser/)）。
  - 字节串拼接需要与 `TypeValidator::getHexDataSize` 相关的长度校验配合，避免折叠后超限。
- **风险**：位运算在 Bitcoin Script 原生指令集里**部分不可用**（OP_AND/OP_OR/OP_XOR 在多数网络被禁用）；但如果是**编译期折叠成字面量**则无所谓，仍然有收益。

#### 3.5 短路运算的常量边界传播

- **问题**：目前常量折叠要求**两侧都是字面量**才折叠 `&&` / `||`。实际上 `false && any` 恒为 `false`、`true || any` 恒为 `true`，无需右侧为字面量。
- **位置**：[constant_folder.cpp tryFoldBinary](../src/compiler/constant_folder.cpp#L308-L311) 前增加一个特例分支；或在 `foldExpr` 里对 `&&/||` 作专门判断。
- **做法要点**：左侧折到 `0 / 非0` 后，若短路成立则整体替换为左侧结果；否则退化为右侧。
- **风险**：若右侧含副作用（调用、require），不能直接丢弃；需要一个"纯表达式"判据（可复用 §3.3 的副作用分析）。保守策略：只对两侧都是标识符/字面量/索引访问时启用。

#### 3.6 Alt-Stack 跨 if-else 分支共享保存精简

- **问题**：`saveSharedAltStack` 在 [ast_to_bytecode_visitor.cpp:372](../src/compiler/ast_to_bytecode_visitor.cpp#L372) 对"跨分支共享变量"成对发射 `OP_TOALTSTACK/OP_FROMALTSTACK`。但如果**某个共享变量在进入的这个分支里根本没被写**，保存-恢复是空开销。
- **位置**：[src/bytecode/symtab.cpp](../src/bytecode/symtab.cpp) 的 setAlt 路径 + [pre_analysis_visitor.cpp](../src/compiler/pre_analysis_visitor.cpp) 补一个 "per-branch write set" 收集。
- **做法要点**：PreAnalysis 阶段为每个 if-else 分支记录被写入的标识符集合 `Writes(branch)`；saveSharedAltStack 只对 `Writes(thenBranch) ∪ Writes(elseBranch)` 的交集-并集运作（只保存实际可能被改的变量）。
- **风险**：write-set 计算要穿透嵌套块、函数调用传参（按引用？本语言目前无此概念，按值所有权模型）；分析偏保守 = 不出错但收益变小，偏乐观 = 出错。先按保守方向实现。

#### 3.7 赋值 `b = a` 的 Alt-Stack 路径自适应

- **问题**：[ast_to_bytecode_visitor.cpp:776-792](../src/compiler/ast_to_bytecode_visitor.cpp#L776-L792)（情形 B：`posA < posB`）当前固定模式：`PICK(posA) ; (SWAP+TOALTSTACK)×posB ; TOALTSTACK ; DROP ; FROMALTSTACK×(posB+1)` —— 字节成本随 `posB` 线性增长（约 `2·posB + 5`）。
- **位置**：同上。
- **做法要点**：对 `posB ≤ 3` 的小距离，考虑替代序列（例如 `PICK(posA) ; ROLL(posB+1) ; DROP`），字节数可能更优。需要跑几组典型 posB 生成两种序列、对比长度，列成 lookup 表。
- **风险**：不同 posA/posB 组合下最优序列不同；要用枚举 + 穷举比较做出决策表，不能凭直觉。应该配测试样例验证栈结果一致。

---

### Tier 3 — 高收益但需基础设施改造

#### 3.8 活跃性分析 + 死变量消除

- **问题**：无任何一处记录"这个局部变量被读过几次 / 在哪里最后一次用"。声明后从未被读的变量仍然会完整经历 push + 作用域清理。
- **位置**：在 [pre_analysis_visitor.h/.cpp](../src/compiler/pre_analysis_visitor.h) 增加一次活跃性扫描，或新增独立 Pass `LivenessAnalysisVisitor` 放在 ConstantFolder 之前/之后。
- **做法要点**：
  - 为每个声明记录 `read_count` 与 `last_use_position`（AST 路径）。
  - `read_count == 0` 的变量：删除声明节点，并连带删除其 RHS（若无副作用）。
  - 扩展 §3.3：让 ExprStmtNode 的副作用分析复用同一套。
- **风险**：语言里可能存在**隐式被读**的场景（例如 `return` 隐式返回栈顶、某些内置函数按名称查栈槽），分析需要覆盖；否则会误删。推荐**先保守：只对 `VarDecl + 纯字面量 RHS` 启用**。

#### 3.9 last_use 提前 DROP

- **问题**：即使变量被读过，一旦最后一次读发生在作用域中段，剩余字节码仍需绕过它（每一条 ROLL 都要多算深度）。
- **位置**：基于 §3.8 的 `last_use_position`。
- **做法要点**：在 last_use 紧后 emit `OP_DROP` 并从 symtab 弹出，后续所有栈位深度减 1。
- **风险**：与当前"作用域退出时一次性清理"的模型耦合较深，要改 Scope 的清理语义。非闭合窗口（闭合 if/else）里的 last_use 要小心。

#### 3.10 结构体字段访问 CSE（公共子表达式消除）

- **问题**：`tx.version + tx.data.inputs + tx.data.outputs` 这类密集字段访问，每次都走完整 `getArrayElementLabel → getPos → emitPick` 链。若同一字段在一个基本块里被读多次，每次都是一次 PICK（字节码上 `<n> OP_PICK`）。
- **位置**：[ast_to_bytecode_visitor.cpp FieldAccess/IndexAccess 处](../src/compiler/ast_to_bytecode_visitor.cpp)、[src/bytecode/scope.h](../src/bytecode/scope.h)。
- **做法要点**：
  - 在 StaticInfoVisitor 阶段统计基本块内 `(base, field-path)` 的出现次数。
  - 对 ≥2 次出现的字段：首访问后保留栈顶副本并在 symtab 注册临时标签；后续访问用 emitPick(浅位置)。
  - 离开基本块时统一 DROP 掉临时副本。
- **风险**：结构体嵌套层级深时，保留栈顶副本本身占栈位，反而抬高其它变量的 pick 深度；需要一个收益模型（访问次数 × 深度差 vs. 维护成本）。

#### 3.11 类型驱动的 PushData 编码选择

- **问题**：[bytecode_helper_fun.h:60-79](../src/bytecode/bytecode_helper_fun.h#L60-L79) 的 PushData 编码选择**只看运行时字节长度**。编译时类型系统里已知长度的类型（`byte[20]`、`hex32`、`Addr`）未被利用来跳过某些长度检查或选择更紧凑的包装。
- **位置**：[src/bytecode/byt_data_types.h](../src/bytecode/byt_data_types.h)、内置函数展开位点（见 [bytecode_builtin_function.h](../src/bytecode/bytecode_builtin_function.h)）。
- **做法要点**：让 Builtin 展开模板在参数类型可在编译期确定长度时，直接落到 `直接长度前缀`；对 Boolean 字面量强制走 OP_0/OP_1。
- **风险**：主要是保证与 `ScriptDecoder` 反汇编一致，以免 `--debug-output` 读回对不上。

#### 3.12 小函数内联

- **问题**：无论 private 函数被调用几次，都会走正常的调用惯例（参数压栈 + 跳转 + 返回）。对**单点调用**或**函数体极小**的私有函数，完全可以在编译期内联展开。
- **位置**：AST 层新增 `InlinerVisitor` Pass（在 ConstantFolder 与 AstToBytecode 之间）。
- **做法要点**：
  - 收集"私有 + 无递归 + 调用点数 ≤ 1 或函数体指令估算 ≤ N"的候选。
  - 内联时把参数换成局部变量声明；保留 return 语义（最后一条表达式结果）。
- **风险**：与 `--asa` 的 setAlt/setMain 子作用域限制交互复杂（参见 [project_doc/STACK_MANAGEMENT_MECHANISM.md](STACK_MANAGEMENT_MECHANISM.md)）。需要在 PreAnalysis 里检查候选是否涉及 alt-stack，涉及的话放弃内联。

#### 3.13 重复字节串的运行时复用（常量池）

- **问题**：同一个公钥 / 哈希值 / 魔数被多处 `PUSH`，每次都完整序列化一份（32-byte pubkey 至少是 33 字节）。
- **位置**：[ast_to_bytecode_visitor.cpp](../src/compiler/ast_to_bytecode_visitor.cpp) visitLiteral + Scope 扩展。
- **做法要点**：在函数序言里对"出现 ≥2 次、长度 ≥10 字节"的字节串字面量先 `PUSH`、再 `OP_TOALTSTACK`；使用点用 `OP_FROMALTSTACK OP_DUP OP_TOALTSTACK`（或再次 PUSH 备份副本）。需要在退出时清理。
- **风险**：Alt-stack 预算紧张时反而不划算；且与 §3.6 的 Alt-Stack 共享保存冲突。建议只作为**短期常量池**（单函数内）。

---

## 四、关键文件索引

| 角色 | 路径 |
|------|------|
| emit 主路径 & 作用域清理 | [src/compiler/ast_to_bytecode_visitor.cpp](../src/compiler/ast_to_bytecode_visitor.cpp) |
| AST 层常量折叠 | [src/compiler/constant_folder.cpp](../src/compiler/constant_folder.cpp) |
| 字节码装配 | [src/bytecode/bytecode_generator.cpp](../src/bytecode/bytecode_generator.cpp) |
| PushData / 小整数编码 | [src/bytecode/bytecode_helper_fun.h](../src/bytecode/bytecode_helper_fun.h) |
| 栈 / 符号表 / Alt-Stack | [src/bytecode/symtab.cpp](../src/bytecode/symtab.cpp)、[src/bytecode/scope.h](../src/bytecode/scope.h) |
| 类型信息 | [src/bytecode/byt_data_types.h](../src/bytecode/byt_data_types.h)、[src/bytecode/type_validator.h](../src/bytecode/type_validator.h) |
| 预分析 / 静态信息 | [src/compiler/pre_analysis_visitor.h](../src/compiler/pre_analysis_visitor.h)、[src/compiler/static_info_visitor.h](../src/compiler/static_info_visitor.h) |
| Pass 注册入口 | [src/pass/pass_manager.cpp](../src/pass/pass_manager.cpp)、[main.cpp:1331-1343](../main.cpp#L1331-L1343) |

## 五、验证与对比建议（给实施者）

1. **基线**：用当前 `main` 对 `test/` 下有代表性的样例分别编译，记录：
   - 最终脚本字节数（`--debug-output` JSON 里的 `bytecode_length`、或直接量输出大小）；
   - 反汇编（通过 `ScriptDecoder`）；
   - BVM simulator 执行结果。
2. **候选样例**：`test/scrypt_ported/*`、`wallet.ct`、`test_1.ct`…`test_4.ct`；特别覆盖：
   - 含多层 if/else 的条件脚本（检验 §3.2、§3.6）；
   - 结构体字段密集访问（检验 §3.10）；
   - 含大量常量表达式的模板脚本（检验 §3.4、§3.5）；
   - 大量局部变量的函数体（检验 §3.8、§3.9）。
3. **等价性**：每一项落地后，对比前后的反汇编 diff 与 simulator 执行结果；任何语义等价不严格成立的改动必须回退。
4. **回归**：确保 `--debug-output` JSON 仍然可被调试器正确加载（断点 / 变量查看 / PC 映射）。

## 六、收益与风险一览

| 条目 | 预期字节节省 | 实施难度 | 主要风险 |
|------|-------------|---------|---------|
| 3.1 字节码 Peephole | 3%-8%（经验估计，需 benchmark 验证） | 中 | 控制流边界、调试 PC 映射 |
| 3.2 死分支消除 | 场景敏感，模板脚本可大 | 低 | 副作用检测需保守 |
| 3.3 ExprStmt 立即 DROP | 小但普遍 | 低 | 依赖副作用分析 |
| 3.4 常量折叠扩展（位/串） | 依赖语法支持度 | 低 | 字节串长度校验 |
| 3.5 短路常量传播 | 小 | 低 | 右侧副作用判据 |
| 3.6 Alt-Stack 分支精简 | 含分支脚本明显 | 中 | per-branch 写集精度 |
| 3.7 赋值 Alt-Stack 自适应 | 赋值密集场景明显 | 中 | 枚举生成+测试 |
| 3.8 活跃性 + 死变量 | 依赖代码风格 | 中-高 | 隐式读路径 |
| 3.9 last_use 提前 DROP | 复合收益（让 ROLL 深度下降） | 高 | 作用域模型改造 |
| 3.10 字段访问 CSE | 结构体密集场景明显 | 高 | 栈位预算 |
| 3.11 类型驱动 PushData | 特定场景 1-2% | 中 | 反汇编一致性 |
| 3.12 小函数内联 | 依赖使用模式 | 中-高 | 与 --asa 交互 |
| 3.13 常量池 | 含重复长字节串时明显 | 中 | 与 3.6 冲突风险 |

## 七、非目标

- 本文档**不提供**基准数据 —— 上表的百分比为经验估计，真实数字需实施后 benchmark。
- 不替代每项优化实施时的 PR 级别设计文档。
- 不涉及**正确性增强**（类型检查、错误信息等），仅关注字节码长度。
- 不讨论 Bitcoin Script 之外的后端（当前编译目标固定）。

## 八、建议的实施顺序

1. 先做 **§3.1 字节码 Peephole**：与现有 AST 层优化解耦，收益稳定，且能放大后续优化的增量。
2. 再做 **§3.2 死分支消除**：利用已有 ConstantFolder 成果，改动面小。
3. 然后 **§3.3 / 3.5**：副作用分析一旦建立，多处复用。
4. Tier 3 按团队带宽择机立项，每一项都建议单独 PR 并附 benchmark。
