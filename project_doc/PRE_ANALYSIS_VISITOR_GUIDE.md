# Pre-Analysis Visitor 业务逻辑文档

## 概述

`PreAnalysisVisitor` 是编译器前端的一个重要组件，负责在代码生成之前进行**所有权分析**和**变量生命周期检查**。它确保变量的正确使用，防止所有权冲突和内存安全问题。

## 核心概念

### 1. 变量数据来源分类

```cpp
enum class DataSource {
    CONTRACT_MEMBER,    // 合约成员变量 (self.*, <self.*>)
    CONSTANT_VALUE,     // 常量赋值 (x = "st", x = 10, x = 0x11)
    STACK_DATA,         // 栈上数据 (函数参数、op_function生成的数据)
    BUILTIN_OBJECT      // 内置对象 (BVM, alt等)
};
```

### 2. 变量状态

```cpp
enum class VariableState {
    DECLARED,    // 已声明但未使用
    USED,        // 已使用但未消耗
    CONSUMED     // 已被消耗（所有权转移）
};
```

## 所有权规则

### 核心原则
**只有栈上数据（STACK_DATA）才具有所有权概念，需要进行所有权检查。**

### 变量分类详解

#### 1. 合约成员变量 (CONTRACT_MEMBER)
- **特征**: `self.*` 或 `<self.*>` 模式
- **示例**: `self.pubKey`, `<self.balance>`, `self.owner`
- **所有权**: 无所有权概念，可以任意使用和传递
- **原因**: 这些数据来自合约状态，由区块链管理生命周期

#### 2. 常量值 (CONSTANT_VALUE)
- **特征**: 直接赋值的字面量
- **示例**: `x = "hello"`, `y = 42`, `z = 0x1234`
- **所有权**: 无所有权概念
- **原因**: 编译时已知的常量，不涉及运行时内存管理

#### 3. 内置对象 (BUILTIN_OBJECT)
- **特征**: `bytecode_builtin_struct` 中定义的对象
- **示例**: `BVM`, `alt`, `self`, `BVM.stack`, `alt.stack`
- **所有权**: 无所有权概念
- **原因**: 由虚拟机管理的全局对象，生命周期由系统控制

#### 4. 栈上数据 (STACK_DATA)
- **特征**: 函数参数、局部变量、op_function生成的数据
- **示例**: 函数参数 `signature`, `Hash160(data)` 的返回值
- **所有权**: 具有所有权概念，需要严格的生命周期管理
- **原因**: 这些数据在栈上分配，使用后可能被消耗或转移

## 消耗性操作

### 定义的消耗性操作
```cpp
m_consumingOperations = {
    "Hash160",
    "CheckSig", 
    "Sha256",
    "Ripemd160"
};
```

### 条件表达式中的变量消耗
根据 Bitcoin Script 的 Flow Control 操作特性，`if` 语句会从栈顶弹出一个值来判断条件，因此：

- **`if variableName:`** - 直接使用变量名作为条件会**消耗该变量**
- **`if functionCall():`** - 消耗的是函数返回的匿名临时数据，不影响已声明变量
- **`if self.member:`** - 合约成员变量不受所有权限制，可以安全使用
- **`if expression:`** - 复杂表达式消耗的是表达式结果

### 消耗性操作的影响
- 当栈上数据被传递给消耗性操作时，该变量的所有权被转移
- 当栈上数据被用作 if 条件时，该变量的所有权被转移
- 变量状态变为 `CONSUMED`
- 后续使用该变量将报错

## 使用示例与强壮性验证

### 1. 基础正确使用模式

```python
# 1.1 合约成员变量 - 无所有权限制
Contract VerifyContract:
    def verify(signature: hex) -> bool:
        h160: hex = Hash160(self.pubKey)  # ✅ self.pubKey 是合约成员，可以安全使用
        return CheckSig(signature, h160)

# 1.2 常量值 - 无所有权限制  
Contract ExampleContract:
    def example():
        msg: string = "hello world"  # ✅ 字符串常量
        num: int = 42               # ✅ 数字常量
        hexVal: hex = 0x1234        # ✅ 十六进制常量
        Hash160(msg)                # ✅ 常量可以安全传递给消耗性操作
        Sha256(num)                 # ✅ 多次使用常量都是安全的

# 1.3 内置对象 - 无所有权限制
Contract StackContract:
    def stackOp():
        setAlt(100)      # ✅ setAlt 是内置函数
        setMain(200)     # ✅ setMain 是内置函数
        # 注意：BVM.push/pop 等需要根据实际内置函数调整
```

### 2. 复杂场景的强壮性验证

#### 2.1 混合数据类型的复杂表达式
```python
Contract ComplexContract:
    def complexMixedTypes(userSig: hex, userPubKey: hex):
        # 混合使用不同类型的数据
        contractHash: hex = Hash160(self.pubKey)     # ✅ 合约成员
        constantHash: hex = Hash160("fixed_string")  # ✅ 常量
        userHash: hex = Hash160(userPubKey)          # ✅ 栈数据第一次使用
        
        # 验证签名 - 混合使用
        result1: bool = CheckSig(userSig, contractHash)  # ✅ userSig被消耗，contractHash无所有权限制
        result2: bool = CheckSig("dummy_sig", userHash)  # ✅ 常量和已消耗的栈数据
        
        # 尝试再次使用已消耗的栈数据
        # result3: bool = CheckSig(userSig, contractHash)  # ❌ userSig已被消耗
        
        return result1 and result2
```

#### 2.2 嵌套字段访问的处理
```python
Contract NestedContract:
    def nestedFieldAccess():
        # 深层嵌套的合约成员访问
        value1: hex = Hash160(self.config.publicKey)    # ✅ 深层合约成员
        value2: hex = Sha256(self.state.balance)        # ✅ 合约成员字段访问
        
        # 注意：根据语法规范，内置对象的具体API需要确认
        # 这里使用setAlt/setMain作为示例
        setAlt(value1)                                  # ✅ 内置函数调用
        setMain(value2)                                 # ✅ 内置函数调用
```

#### 2.3 条件分支中的所有权处理
```python
Contract ConditionalContract:
    def conditionalOwnership(data: hex, flag: bool):
        if flag:
            # 在条件分支中使用栈数据
            hash1: hex = Hash160(data)           # ✅ 第一次使用
            result: bool = CheckSig(hash1, self.pubKey) # ✅ hash1被消耗，self.pubKey无限制
        else:
            # 在另一个分支中使用相同的栈数据
            hash2: hex = Sha256(data)            # ✅ 在不同分支中使用是安全的
            setAlt(hash2)                        # ✅ hash2被传递给内置函数
        
        # 分支外使用 - 这里会有潜在问题
        # finalHash: hex = Hash160(data)        # ❌ 可能的所有权冲突（取决于执行路径）
```

#### 2.4 if 条件表达式中的变量消耗
```python
Contract IfConditionContract:
    def ifConditionConsumption(publicKey: hex, signature: hex):
        # 情况1：直接使用变量名作为条件 - 会消耗该变量
        if publicKey:                           # ❌ publicKey被if条件消耗
            result1: bool = CheckSig(signature, self.pubKey)
        # 后续使用publicKey会报错
        # hash: hex = Hash160(publicKey)        # ❌ 错误！publicKey已被消耗
        
    def ifConditionWithContractMember():
        # 情况2：使用合约成员作为条件 - 不会消耗
        if self.pubKey:                         # ✅ 合约成员不受所有权限制
            result1: hex = Hash160(self.pubKey) # ✅ 可以继续使用
            result2: hex = Sha256(self.pubKey)  # ✅ 可以多次使用
            
    def ifConditionWithFunctionCall(data: hex):
        # 情况3：使用函数调用作为条件 - 消耗的是返回值，不影响参数
        if Hash160(data):                       # ✅ 消耗的是Hash160的返回值
            result: hex = Sha256(data)          # ✅ data仍然可用
            
    def correctIfUsage(publicKey: hex):
        # 正确做法：先克隆再用作条件
        if publicKey.clone():                   # ✅ 使用克隆值作为条件
            hash: hex = Hash160(publicKey)      # ✅ 原变量仍然可用
```

### 3. 错误模式与检测能力

#### 3.1 典型的所有权冲突
```python
Contract OwnershipConflictContract:
    def ownershipConflict(signature: hex, publicKey: hex):
        # 错误：重复消耗栈数据
        hash1: hex = Hash160(publicKey)          # ✅ 第一次使用
        # hash2: hex = Hash160(publicKey)        # ❌ 错误！publicKey已被消耗
        
        # 错误：使用已消耗的数据
        sig_check1: bool = CheckSig(signature, hash1) # ✅ 第一次使用signature
        # sig_check2: bool = CheckSig(signature, hash1) # ❌ 错误！signature已被消耗
```

#### 3.2 变量声明冲突检测
```python
Contract DeclarationConflictContract:
    def declarationConflicts():
        data: string = "hello"           # ✅ 第一次声明
        # data: int = 42                 # ❌ 错误！重复声明变量
        
    def parameterConflict(param: hex):
        # param: string = "new_value"    # ❌ 错误！参数重复声明
        result: hex = Hash160(param)     # ✅ 正确使用参数
```

#### 3.3 未声明变量检测
```python
Contract UndeclaredVariableContract:
    def undeclaredVariables():
        # result: hex = Hash160(unknownVar)  # ❌ 错误！未声明的变量
        
        # if someCondition:                  # ❌ 错误！未声明的变量
        #     setAlt(100)
        
        # 正确的做法
        condition: bool = True
        if condition:
            setAlt(100)                    # ✅ 正确使用
```

### 4. 边界情况的强壮性

#### 4.1 空值和特殊字符处理
```python
Contract EdgeCaseContract:
    def edgeCases():
        # 特殊字符在变量名中
        self_data: hex = Hash160(self.pubKey)    # ✅ 下划线变量名
        
        # 空字符串常量
        empty: string = ""                       # ✅ 空字符串常量
        hash_empty: hex = Hash160(empty)         # ✅ 可以安全使用
        
        # 零值常量
        zero: int = 0                           # ✅ 零值常量
        hash_zero: hex = Sha256(zero)           # ✅ 可以安全使用
```

#### 4.2 复杂的表达式组合
```python
Contract ComplexExpressionContract:
    def complexExpressions(a: hex, b: hex):
        # 复杂的右值表达式
        result: hex = Hash160(a + b)            # ✅ 表达式中的栈数据使用
        
        # 嵌套函数调用
        nested: bool = CheckSig(
            Hash160(a),                         # ✅ a被Hash160消耗
            Sha256(self.pubKey)                 # ✅ 合约成员可以安全使用
        )
        
        # 后续使用已消耗的变量
        # another: hex = Hash160(a)            # ❌ 错误！a已被消耗
```

### 5. 性能和扩展性验证

#### 5.1 大量变量的处理
```python
Contract PerformanceTestContract:
    def manyVariables():
        # 声明大量变量测试性能
        var1: string = "constant1"
        var2: string = "constant2"
        var3: string = "constant3"
        # ... 可以处理大量变量而不影响性能
        var100: string = "constant100"
        
        # 所有常量都可以安全使用
        Hash160(var1)
        Sha256(var2)
        Ripemd160(var3)
        # ... 
        Hash160(var100)
```

#### 5.2 深度嵌套的作用域
```python
Contract DeepNestingContract:
    def deepNesting(param1: hex):
        condition1: bool = True
        condition2: bool = True
        condition3: bool = True
        
        if condition1:
            local1: hex = Hash160(param1)       # ✅ 在嵌套中使用参数
            
            if condition2:
                local2: hex = Sha256(self.data) # ✅ 合约成员在深度嵌套中
                
                if condition3:
                    # 更深层的嵌套
                    setAlt(local2)              # ✅ local2被传递给内置函数
                    
                    # 尝试再次使用
                    # setMain(local2)          # ❌ 错误！local2已被消耗
```

### 6. 实际应用场景验证

#### 6.1 典型的支付验证函数
```python
Contract PaymentContract:
    def paymentVerification(signature: hex, amount: int, recipient: hex) -> bool:
        # 验证签名
        senderHash: hex = Hash160(self.pubKey)           # ✅ 合约成员
        sigValid: bool = CheckSig(signature, senderHash) # ✅ signature被消耗
        
        # 验证金额
        minAmount: int = 1000                           # ✅ 常量
        amountValid: bool = (amount >= minAmount)       # ✅ 比较操作
        
        # 验证接收者
        recipientHash: hex = Hash160(recipient)         # ✅ recipient被消耗
        
        # 最终验证
        return sigValid and amountValid and (recipientHash != senderHash)
        
        # 错误示例：尝试重复使用已消耗的数据
        # logHash: hex = Hash160(signature)            # ❌ 错误！signature已被消耗
        # backupHash: hex = Hash160(recipient)         # ❌ 错误！recipient已被消耗
```

#### 6.2 多重签名验证
```python
Contract MultiSigContract:
    def multiSigVerification(sig1: hex, sig2: hex, sig3: hex, 
                           pubKey1: hex, pubKey2: hex, pubKey3: hex) -> bool:
        # 验证第一个签名
        hash1: hex = Hash160(pubKey1)                   # ✅ pubKey1被消耗
        valid1: bool = CheckSig(sig1, hash1)            # ✅ sig1被消耗
        
        # 验证第二个签名
        hash2: hex = Hash160(pubKey2)                   # ✅ pubKey2被消耗  
        valid2: bool = CheckSig(sig2, hash2)            # ✅ sig2被消耗
        
        # 验证第三个签名
        hash3: hex = Hash160(pubKey3)                   # ✅ pubKey3被消耗
        valid3: bool = CheckSig(sig3, hash3)            # ✅ sig3被消耗
        
        # 所有签名都必须有效
        return valid1 and valid2 and valid3
        
        # 错误示例：尝试重复使用
        # backup1: bool = CheckSig(sig1, self.backupKey) # ❌ 错误！sig1已被消耗
```

## 强壮性总结

前置检查器展现出以下强壮性特征：

### ✅ **正确识别能力**
- 准确区分4种数据来源类型
- 正确处理复杂的嵌套字段访问
- 识别各种形式的常量值

### ✅ **错误检测能力**  
- 检测所有权冲突和重复使用
- 发现未声明变量的使用
- 识别重复声明的变量

### ✅ **边界情况处理**
- 处理空值和特殊字符
- 支持深度嵌套的作用域
- 处理复杂的表达式组合

### ✅ **性能和扩展性**
- 高效处理大量变量
- O(log n) 的变量查找复杂度
- 简化的作用域管理避免性能瓶颈

### ✅ **实用性验证**
- 支持真实的支付验证场景
- 处理复杂的多重签名逻辑
- 提供清晰的错误信息指导修复

## 检查流程

### 1. 变量声明时
```cpp
DataSource classifyVariable(name, initValue) {
    if (isContractMember(name)) return CONTRACT_MEMBER;
    if (isBuiltinObject(name)) return BUILTIN_OBJECT;  
    if (initValue && isConstantValue(initValue)) return CONSTANT_VALUE;
    return STACK_DATA;  // 默认为栈上数据
}
```

### 2. 变量使用时
```cpp
void useVariable(name, location) {
    // 1. 检查是否为合约成员或内置对象
    if (isContractMember(name) || isBuiltinObject(name)) {
        return;  // 直接跳过所有权检查
    }
    
    // 2. 查找变量定义
    var = findVariable(name);
    
    // 3. 只对栈上数据进行所有权检查
    if (!var->hasOwnership()) return;
    
    // 4. 检查是否已被消耗
    if (var->state == CONSUMED) {
        reportError("Variable has been consumed");
    }
}
```

### 3. 变量消耗时
```cpp
void consumeVariable(name, location) {
    // 同样的检查流程，但将状态设置为 CONSUMED
}
```

## 错误类型

### 1. 未声明变量
```
Error: Undeclared variable: 'varName'
```

### 2. 重复声明
```  
Error: Variable 'varName' redeclared
```

### 3. 所有权冲突
```
Error: Variable 'varName' has been consumed and cannot be used again
```

### 4. 未使用变量警告
```
Warning: Variable 'varName' declared but not used
```

## 实现特点

### 1. 简化的作用域管理
- 不维护复杂的作用域栈
- 每个函数开始时清空变量列表
- 只关注当前函数内的变量生命周期

### 2. 高效的变量查找
- 使用 `std::map<std::string, VariableInfo>` 存储变量
- O(log n) 的查找复杂度

### 3. 统一的错误处理
- 所有错误通过 `ErrorManager` 统一报告
- 区分错误和警告的严重程度

## 扩展性

### 添加新的消耗性操作
```cpp
// 在构造函数中添加
m_consumingOperations.insert("NewConsumingOp");
```

### 添加新的内置对象
```cpp
bool isBuiltinObject(const std::string& varName) {
    // 添加新的内置对象检查逻辑
    if (varName == "NewBuiltinObject") return true;
    // ...
}
```

## 总结

`PreAnalysisVisitor` 通过清晰的变量分类和简化的所有权规则，确保了代码的内存安全性。它的核心思想是：

1. **明确区分数据来源** - 不同来源的数据有不同的生命周期管理规则
2. **简化所有权概念** - 只有栈上数据才需要所有权检查
3. **统一错误处理** - 提供清晰的错误信息帮助开发者定位问题

这种设计既保证了内存安全，又避免了过度复杂的分析逻辑，是一个平衡性能和安全性的良好方案。