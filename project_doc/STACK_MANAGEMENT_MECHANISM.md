# 编译器栈管理机制详解

## 🎯 **核心设计理念**

**栈顶作为存储和运算单元**：主栈的栈顶是所有计算和操作的核心，临时存放操作数，完成运算后清空。这种设计确保了：
- 所有中间计算结果都在栈顶进行
- 操作数的获取和处理遵循统一的 push/pop 模式
- 栈的状态始终保持可预测和可追踪

## 📋 **栈管理核心接口**

### **基础栈操作**
```cpp
// 核心栈操作方法
m_scopePtr->push(data, type, hex);     // 推入栈顶
auto element = m_scopePtr->pop();      // 从栈顶弹出
m_scopePtr->roll(position);            // 将指定位置元素滚到栈顶  
auto pos = m_scopePtr->getPos(name);   // 获取变量栈位置
```

### **栈状态追踪**
```cpp
// 栈状态格式
stack status: | 0: "data"-"type" | 1: "data"-"type" | ...
// 位置0 = 栈顶，数字递增 = 向栈底方向
```

## 🔄 **AST节点处理机制**

### **1. 赋值节点 (AssignNode) - `x = y`**

**处理顺序：右值 → 左值 → 赋值操作**

```cpp
void ASTToBytecodeVisitor::visit(AssignNode& node)
{
    // 第一步：处理右值表达式
    LOG_DEBUG("Evaluating assignment expression");
    visitExpr(*node.value);                    // 计算右值，结果推入栈顶
    auto valueElementOpt = m_scopePtr->pop();  // 弹出右值结果
    
    // 第二步：处理左值标识符  
    visitExpr(*node.name);                     // 处理左值，变量名推入栈顶
    auto nameElementOpt = m_scopePtr->pop();   // 弹出变量名
    
    // 第三步：执行赋值逻辑
    // 检查是否需要ROLL操作来获取正确的栈位置
    if (COMPILER_STACK_PLACEHOLDERS != valueElementStr) {
        auto elementPosOpt = m_scopePtr->getPos(valueElementStr);
        if (elementPosOpt.has_value() && STACK_TOP_POS != elementPosOpt.value()) {
            // 生成 ROLL 操作将目标数据移到栈顶
            m_generator.emit(numberToScriptHex(elementPosOpt.value()));
            m_generator.emit(tbc::BytOpcode::OP_ROLL);
        }
    }
}
```

**栈变化示例**：
```
x = Push(42)
1. visitExpr(Push(42)) → 栈: [占位符] (Push生成操作码)
2. pop() → 栈: []
3. visitExpr(x) → 栈: [x]  
4. pop() → 栈: []
5. 最终: x在栈上，生成字节码 012a
```

### **2. 字面量节点 (LiteralNode)**

**所有字面量统一转换为hex格式推入栈顶**

```cpp
void ASTToBytecodeVisitor::visitLiteral(LiteralNode& node)
{
    switch (node.type) {
        case LiteralNode::Type::Number: {
            int64_t numberValue = std::stoll(node.value);
            std::string numberHex = numberToScriptHex(numberValue);
            LOG_DEBUG("Pushing number to stack, hex: " + numberHex);
            m_scopePtr->push(numberHex, "num", numberHex);  // 推入栈顶
            break;
        }
        case LiteralNode::Type::String: {
            std::string stringHex = stringToScriptHex(node.value);
            LOG_DEBUG("Pushing string to stack, hex: " + stringHex);
            m_scopePtr->push(stringHex, "string", stringHex);  // 推入栈顶
            break;
        }
        case LiteralNode::Type::Hex: {
            auto hexOpValue = hexToScriptHex(node.value);
            LOG_DEBUG("Pushing hex data to stack, value: " + hexOpValue);
            m_scopePtr->push(hexOpValue, "hex", hexOpValue);  // 推入栈顶
            break;
        }
    }
}
```

**数据转换规则**：
- `0` → `"0x00"` (OP_0)
- `42` → `"0x012a"` (PUSHDATA 42)  
- `"hello"` → `"0x0568656c6c6f"` (PUSHDATA "hello")
- `0x1234` → `"0x021234"` (PUSHDATA 0x1234)

### **3. 标识符节点 (IdentifierNode)**

**变量名或固定值推入栈顶**

```cpp
void ASTToBytecodeVisitor::visitIdentifier(IdentifierNode& node)
{
    // 检查是否为固定值变量
    if (auto fixedVar = m_scopePtr->getFixed(node.name)) {
        LOG_INFO("This is a fixed value variable:", fixedVar.value().getName());
        m_scopePtr->push(fixedVar.value());  // 推入固定值
        return;
    }
    
    // 普通变量名推入栈顶
    LOG_DEBUG("accessing identifier '" + node.name + "'");
    m_scopePtr->push(node.name, "");  // 推入变量名
}
```

**处理逻辑**：
- **固定值变量**：直接推入实际数据
- **栈变量**：推入变量名，后续通过getPos()查找位置
- **特殊标识符**：如"self"等特殊处理

### **4. 函数调用节点 (CallNode)**

**参数逐个处理，函数执行，结果推入占位符**

```cpp
void ASTToBytecodeVisitor::processGenericFunctionCall(...)
{
    // 处理参数：每个参数都经过 推入栈顶 → 立即弹出 的过程
    auto processedArgs = processArguments(args, expectedArgCount, functionName);
    
    // 生成函数操作码
    auto opcodeHex = builtFunPtr->getOpcodeHex(objectElement, processedArgs, m_scopePtr);
    
    // 推入返回值占位符
    for (size_t i = 0; i < builtFunPtr->getReturnCount(); ++i) {
        m_scopePtr->push(COMPILER_STACK_PLACEHOLDERS, "");  // 推入占位符
    }
    
    // 生成字节码
    m_generator.emit(opcodeHex);
}

std::vector<tbc::StackElement> ASTToBytecodeVisitor::processArguments(...)
{
    std::vector<tbc::StackElement> processedArgs;
    
    for (const auto& argElement : args) {
        visitExpr(*args[i]);                    // 参数表达式推入栈顶
        auto argResultOpt = m_scopePtr->pop();  // 立即弹出参数
        processedArgs.push_back(argResultOpt.value());
    }
    
    return processedArgs;
}
```

**函数调用栈变化**：
```
Push(42) 调用过程：
1. 处理参数42 → 栈: [0x012a]
2. pop参数 → 栈: [] → 参数传给Push函数  
3. Push生成操作码012a → 推入占位符 → 栈: [占位符]
4. 赋值处理时pop占位符 → 栈: []
```

## 🎯 **特殊操作的栈管理**

### **1. 消耗性函数处理**

**消耗性函数会从栈中移除被消耗的变量**

```cpp
// Delete函数的特殊处理
if ("Delete" == functionName) {
    // 先处理fixed数据删除
    for (const auto& arg : args) {
        if (auto identifierNode = dynamic_cast<const IdentifierNode*>(arg.get())) {
            std::string varName = identifierNode->name;
            if (auto fixedVar = m_scopePtr->getFixed(varName)) {
                m_scopePtr->removeFixed(varName);  // 移除固定值
            }
        }
    }
}

// 消耗性方法调用的对象处理
if (isConsumeFun(functionName) && isMethodCall) {
    auto objPosOpt = m_scopePtr->getPos(objectElement.value());
    if (objPosOpt.has_value()) {
        m_scopePtr->roll(objPosOpt.value());  // 将对象滚到栈顶
        m_scopePtr->pop();                    // 弹出消耗对象
    }
}
```

### **2. ROLL操作的字节码生成**

**当需要访问非栈顶位置的数据时，生成ROLL操作**

```cpp
// 检查变量是否在栈顶位置
auto elementPosOpt = m_scopePtr->getPos(valueElementStr);
if (elementPosOpt.has_value() && STACK_TOP_POS != elementPosOpt.value()) {
    // 生成位置推送操作码
    LOG_DEBUG("Emitting: ", numberToScriptHex(elementPosOpt.value()));
    m_generator.emit(numberToScriptHex(elementPosOpt.value()));
    
    // 生成ROLL操作码
    LOG_DEBUG("Emitting: OP_ROLL");
    m_generator.emit(tbc::BytOpcode::OP_ROLL);
    
    LOG_INFO("The variable \"", valueElementStr, "\" becomes invalid");
}
```

**ROLL操作示例**：
```
栈状态: | 0: A | 1: B | 2: C | 3: D |
访问变量C (位置2):
1. 推送2: | 0: 2 | 1: A | 2: B | 3: C | 4: D |
2. OP_ROLL: | 0: C | 1: A | 2: B | 3: D |
生成字节码: 52 7a (OP_2 OP_ROLL)
```

## 📊 **栈状态跟踪与验证**

### **日志模式识别**

```cpp
// 标准栈操作日志格式
[DEBUG] stack:0x[地址] push:[数据]
[DEBUG] stack:0x[地址] status: | 0: "数据"-"类型" | 1: "数据"-"类型" | ...
[DEBUG] stack:0x[地址] pop:[数据]
```

### **栈平衡验证规则**

1. **基本平衡**：每个push都应有对应的pop
2. **函数调用平衡**：参数处理前后栈深度变化合理
3. **占位符清理**：`/CompilerStackPlaceholders/`及时清理
4. **变量分配**：赋值后变量正确进入栈追踪

### **if/else 分支状态合并**

固定区只保存编译期已知值，不是运行时 VM 栈。外层变量在不同分支被重新绑定时，不能在 `OP_ENDIF` 后继续选择某一条分支的固定值，否则会把条件结果错误地编译成常量。

当前合并流程如下：

1. 在执行分支前保存入口符号表和独立的副栈快照。
2. 分别从同一个入口状态编译 `then` 和 `else`。
3. 收集两条分支中被赋值的外层存储槽；分支内新声明的局部变量不参与合并。
4. 在每条可继续执行的分支末尾，将待合并变量规范化到主栈顶：
   - 固定区值：发射对应常量并绑定为运行时主栈槽。
   - 主栈值：通过 `ROLL` 调整到统一顺序。
   - 副栈值：移回主栈，同时保持其他副栈元素的顺序。
5. 验证两条分支的主栈和副栈布局一致，再重建父作用域状态。
6. 若某条分支必然 `Return`，后续状态只采用仍可到达 `OP_ENDIF` 的分支。

例如：

```text
x: int = 0
if flag > 0:
    x = 1
else:
    x = 2
Return(x)
```

合并后的关键字节码应为：

```text
OP_IF OP_1 OP_ELSE OP_2 OP_ENDIF OP_RETURN
```

`OP_1` 和 `OP_2` 必须位于各自分支内；`OP_ENDIF` 后的 `Return(x)` 直接使用合并后的运行时栈槽。

### **常见栈状态模式**

```cpp
// 字面量处理模式
[] → [字面量] → [] (立即被pop)

// 变量赋值模式  
[] → [右值] → [右值,左值] → [] → [变量在栈]

// 函数调用模式
[] → [参数1] → [参数2] → ... → [] → [占位符] → []

// 变量访问模式
[...其他变量] → [目标变量滚到栈顶] → [处理后状态]
```

## 🔍 **调试和问题排查**

### **常见问题类型**

1. **栈不平衡**
   - **症状**：push/pop不匹配，栈深度异常
   - **排查**：检查每个操作的push/pop配对

2. **位置计算错误**  
   - **症状**：ROLL操作访问错误位置
   - **排查**：验证getPos()返回值与实际栈状态

3. **占位符残留**
   - **症状**：`/CompilerStackPlaceholders/`未清理
   - **排查**：检查函数调用后的栈清理逻辑

4. **数据类型错误**
   - **症状**：字面量转换或类型标识不正确
   - **排查**：验证visitLiteral()的转换逻辑

### **验证检查清单**

✅ **基础验证**
- [ ] 每个AST节点处理都有完整的push/pop序列
- [ ] 栈状态日志连续且逻辑正确
- [ ] 字面量到hex的转换准确

✅ **操作验证**  
- [ ] 赋值操作严格按右值→左值→赋值顺序
- [ ] 函数调用参数逐个处理，占位符正确管理
- [ ] ROLL操作的位置计算与实际栈状态匹配

✅ **结果验证**
- [ ] 最终生成的字节码与栈操作逻辑一致
- [ ] 栈变量分配合理，无遗漏或重复
- [ ] 消耗性函数正确处理变量生命周期

---

## 📝 **总结**

编译器的栈管理机制围绕"栈顶作为运算单元"的核心理念设计，通过统一的push/pop操作模式，确保所有AST节点的处理都遵循相同的栈管理规则。这种设计不仅保证了代码的一致性和可维护性，还提供了清晰的调试追踪能力。

**关键设计原则**：
- **统一接口**：所有操作都通过标准的栈接口进行
- **即时处理**：数据推入栈顶后立即处理，避免长期堆积
- **位置追踪**：精确跟踪每个变量在栈中的位置
- **生命周期管理**：明确处理变量的创建、使用和销毁

这套机制为编译器提供了可靠的基础设施，支持复杂的语言特性同时保持实现的清晰和可预测性。
