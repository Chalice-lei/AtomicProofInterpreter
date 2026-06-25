# 语法示例文档

本文档提供了严格基于语法规范的代码示例，展示如何正确使用该语言编写业务代码。

## 1. 基础合约示例

### 1.1 简单的身份验证合约
```python
Contract IdentityContract:

    Struct UserInfo:
        name: string
        pubkey: hex
        age: int

    def __init__(admin_key: hex):
        self.admin = admin_key

    def verify_user(user: UserInfo, signature: string) -> bool:
        result: bool = CheckSig(signature, user.pubkey)
        return result
```

**说明：**
- 定义了结构体 `UserInfo` 包含用户基本信息
- 构造函数初始化管理员公钥
- 验证函数接收用户信息和签名，返回验证结果

### 1.2 数字资产转账合约
```python
Contract TransferContract:

    def __init__(owner_pubkey: hex):
        self.owner = owner_pubkey

    def transfer(from_key: hex, to_key: hex, amount: int, signature: string) -> bool:
        from_key_clone: hex = from_key.Clone()
        signature_clone: string = signature.Clone()
        
        # self.owner 是合约成员变量，可以多次使用无需 Clone
        is_owner: bool = (from_key_clone == self.owner)
        is_valid_sig: bool = CheckSig(signature_clone, from_key)
        is_positive_amount: bool = (amount > 0)
        
        if is_owner:
            if is_valid_sig:
                if is_positive_amount:
                    return true
                else:
                    return false
            else:
                return false
        else:
            return false
```

**说明：**
- 转账合约验证所有者身份
- 使用 `Clone()` 避免变量被消耗，确保可以多次使用
- 演示了所有权系统中的正确变量使用方式

## 2. 结构体和字段访问示例

### 2.1 嵌套结构体示例
```python
Contract UserManagement:

    Struct Address:
        street: string
        city: string
        zipcode: string

    Struct User:
        name: string
        age: int
        address: Address
        balance: int

    def __init__(admin_name: string):
        self.admin_name = admin_name

    def process_user(user: User) -> bool:
        user_clone: User = user.Clone()
        
        user_name: string = user.name
        user_city: string = user_clone.address.city
        
        # 需要再次克隆来访问balance字段
        user_clone2: User = user_clone.Clone()
        current_balance: int = user_clone2.balance
        user_clone.balance = current_balance + 100
        
        if user_clone.age >= 18:
            return true
        else:
            return false

    def update_user_address(user: User, new_street: string):
        user.address.street = new_street
```

**说明：**
- 嵌套结构体定义（Address 嵌套在 User 中）
- 多层字段访问（`user.address.city`）
- 字段赋值操作
- 条件判断和返回值

## 3. 表达式和运算示例

### 3.1 数学运算合约
```python
Contract MathOperations:

    def calculate(a: int, b: int, c: int) -> int:
        sum_result: int = a + b + c
        product: int = a * b
        difference: int = a - b
        quotient: int = a / b
        
        is_greater: bool = (a > b)
        is_equal: bool = (a == b)
        is_not_equal: bool = (a != c)
        
        complex_result: int = (a + b) * c - (a / b)
        
        return complex_result

    def validate_Range(value: int, min_val: int, max_val: int) -> bool:
        min_check: bool = (value >= min_val)
        max_check: bool = (value <= max_val)
        
        if min_check:
            if max_check:
                return true
            else:
                return false
        else:
            return false
```

**说明：**
- 各种算术运算符（+, -, *, /）
- 比较运算符（>, ==, !=, >=, <=）
- 复合表达式和括号使用
- 由于语法规范中没有逻辑运算符（and, or），使用嵌套if实现逻辑判断

## 4. 函数调用和方法调用示例

### 4.1 链式字段访问示例
```python
Contract ChainedAccess:

    Struct Config:
        value: int
        name: string

    Struct System:
        config: Config
        active: bool

    def __init__(initial_value: int):
        self.default_value = initial_value

    def process_system(sys: System) -> int:
        sys_clone: System = sys.Clone()
        
        config_value: int = sys.config.value
        config_name: string = sys_clone.config.name
        
        if sys_clone.active:
            # self.default_value 是合约成员变量，可以多次使用无需 Clone
            result: int = config_value + self.default_value
            return result
        else:
            return 0

    def update_system_config(sys: System, new_value: int, new_name: string):
        sys_clone: System = sys.Clone()
        
        sys.config.value = new_value
        sys_clone.config.name = new_name
        sys_clone.active = true
```

**说明：**
- 链式字段访问（`sys.config.value`）
- 结构体作为参数传递
- 字段赋值和读取
- 条件判断中的字段访问

## 5. 完整的实际应用示例

### 5.1 简单的投票合约
```python
Contract VotingContract:

    Struct Voter:
        pubkey: hex
        has_voted: bool
        vote_choice: int

    Struct Proposal:
        id: int
        title: string
        yes_votes: int
        no_votes: int

    def __init__(admin_key: hex):
        self.admin = admin_key
        self.total_voters = 0

    def register_voter(voter_key: hex) -> bool:
        # self.total_voters 是合约成员变量，可以多次使用无需 Clone
        self.total_voters = self.total_voters + 1
        return true

    def cast_vote(voter: Voter, proposal: Proposal, choice: int, signature: string) -> bool:
        voter_clone: Voter = voter.Clone()
        proposal_clone: Proposal = proposal.Clone()
        
        is_valid_sig: bool = CheckSig(signature, voter.pubkey)
        has_not_voted: bool = (voter_clone.has_voted == false)
        valid_choice: bool = (choice == 0)
        
        if is_valid_sig:
            if has_not_voted:
                if valid_choice:
                    proposal_clone2: Proposal = proposal_clone.Clone()
                    current_no_votes: int = proposal_clone2.no_votes
                    proposal_clone.no_votes = current_no_votes + 1
                    voter_clone.has_voted = true
                    return true
                else:
                    choice_is_one: bool = (choice == 1)
                    if choice_is_one:
                        proposal_clone3: Proposal = proposal_clone.Clone()
                        current_yes_votes: int = proposal_clone3.yes_votes
                        proposal_clone.yes_votes = current_yes_votes + 1
                        voter_clone.has_voted = true
                        return true
                    else:
                        return false
            else:
                return false
        else:
            return false

    def get_result(proposal: Proposal) -> int:
        proposal_clone: Proposal = proposal.Clone()
        yes_votes: int = proposal.yes_votes
        no_votes: int = proposal_clone.no_votes
        
        yes_wins: bool = (yes_votes > no_votes)
        if yes_wins:
            return 1
        else:
            return 0
```

**说明：**
- 实际的投票业务逻辑
- 多个结构体的使用
- 复杂的条件判断逻辑
- 字段的读取和修改
- 函数调用和返回值

## 6. 所有权系统和特殊内置函数示例

### 6.1 所有权系统示例
```python
Contract OwnershipExample:

    Struct UserInfo:
        name: string
        age: int

    def demonstrate_ownership(value1: int, value2: int) -> int:
        cloned_value1: int = value1.Clone()
        
        result1: int = value1 + value2
        result2: int = cloned_value1 * 2
        
        return result1 + result2

    def demonstrate_field_access(user: UserInfo) -> string:
        user_clone: UserInfo = user.Clone()
        
        name1: string = user.name
        name2: string = user_clone.name
        
        return name1

    def incorrect_field_access(user: UserInfo) -> string:
        name1: string = user.name
        # name2: string = user.name  # 错误！user.name 已被消耗
        
        return name1
```

**说明：**
- 使用 `Clone()` 创建变量副本，避免所有权转移
- **字段访问会消耗该字段的所有权**，需要克隆结构体才能多次访问同一字段
- 演示了正确和错误的字段访问方式

### 6.2 特殊内置函数示例
```python
Contract StackOperations:

    def stack_management(data1: hex, data2: hex, data3: hex):
        SetAlt(data1)
        SetAlt(data2)

        SetMain(data1)
        SetMain(data2)
        
        result: hex = data1 + data2 + data3

    def complex_stack_operations(value: int) -> int:
        cloned_value: int = value.Clone()

        SetAlt(value)
        SetMain(cloned_value)

        final_result: int = value + cloned_value
        return final_result
```

**说明：**
- `SetAlt()` 和 `SetMain()` 不会消耗变量的所有权
- 可以多次调用这些函数而不影响变量的可用性
- 演示了栈操作的正确使用方式

## 7. 语法特性总结

通过以上示例展示了语言的核心特性：

### 7.1 支持的语法特性
1. **强类型系统**: 所有变量、参数、返回值都必须声明类型
2. **结构体定义**: 支持自定义数据结构和嵌套结构体
3. **字段访问**: 支持多层字段访问（`obj.field1.field2`），但会消耗字段所有权
4. **函数定义**: 支持带参数和返回值的函数
5. **构造函数**: 使用 `__init__` 定义构造函数
6. **条件控制**: if-else 分支控制
7. **表达式运算**: 算术运算、比较运算
8. **变量声明**: 支持类型注解的变量声明
9. **赋值操作**: 支持变量和字段的赋值
10. **所有权系统**: 变量具有消耗性，支持克隆操作和特殊内置函数

### 7.2 语法限制
1. **无逻辑运算符**: 没有 `and`, `or`, `not` 操作符，需要用嵌套if实现
2. **无循环结构**: 没有 `for`, `while` 循环
3. **无数组/列表**: 没有数组或列表数据结构
4. **无注释语法**: 语法规范中未定义注释
5. **无异常处理**: 没有 try-catch 异常处理机制
6. **单合约系统**: 每个文件只能包含一个合约
7. **字段访问消耗性**: 访问结构体字段会消耗该字段的所有权，需要谨慎使用克隆操作

### 7.3 所有权系统重要提醒
- **合约成员变量例外**: 合约成员变量（如 `self.field`）不受所有权约束，可以多次使用无需 Clone
- **局部变量和参数**: 局部变量和函数参数遵循所有权规则，使用后会被消耗
- **结构体字段访问**: 访问结构体实例的字段会消耗该字段的所有权，无法再次访问
- **需要克隆来多次访问**: 如果需要多次访问同一个局部变量或结构体字段，必须先克隆
- **特殊函数不消耗**: 只有 `SetAlt()` 和 `SetMain()` 不会消耗变量所有权
- **克隆是深拷贝**: `Clone()` 会创建完整的副本，包括嵌套结构体

### 7.4 合约成员变量 vs 局部变量示例
```python
Contract OwnershipComparison:

    def __init__(initial_value: int):
        self.member_value = initial_value

    def demonstrate_difference(local_value: int) -> int:
        # 合约成员变量可以多次使用
        result1: int = self.member_value + 10
        result2: int = self.member_value * 2
        result3: int = self.member_value - 5
        
        # 局部变量需要克隆才能多次使用
        local_clone1: int = local_value.Clone()
        local_clone2: int = local_value.Clone()
        
        local_result1: int = local_value + 10
        local_result2: int = local_clone1 * 2
        local_result3: int = local_clone2 - 5
        
        return result1 + result2 + result3 + local_result1 + local_result2 + local_result3
```

## 8. 库 (Library) 与模板复用

### 8.1 定义一个库模板

库文件包含可被任意用户合约 import 复用的函数和结构体，不含 `main` 或构造函数。库函数允许引用 `self.<field>`——它会被解析为导入该库的合约的同名成员变量，作为一种"约定式契约"使用。

```python
# stdlib/std/p2pkh.ct — P2PKH 标准验证库
Library std.p2pkh:

    def verifyP2PKH(signature: hex, pubKey: hex):
        EqualVerify(Hash160(pubKey.Clone()), self.pubKeyHash)
        result = CheckSig(signature, pubKey)
        return result
```

**说明：**
- 库名 `std.p2pkh` 为 dotted path，与 `import` 解析路径一致
- `verifyP2PKH` 接收两个参数：签名、公钥；内部直接引用 `self.pubKeyHash`
- 返回 `CheckSig` 的布尔结果，供调用方进一步处理
- 约定：使用本模板的合约必须声明 `self.pubKeyHash` 成员变量，否则编译期符号解析会失败

### 8.2 在用户合约中使用库

```python
import std.p2pkh

Contract MyWallet:

    def main(signature: hex, pubKey: hex):
        ok = verifyP2PKH(signature, pubKey)
```

**说明：**
- `import std.p2pkh` 将库的源码内联展开到用户文件
- 编译器自动将库函数合并到合约内部，用户可直接按名称调用
- `ok = verifyP2PKH(...)` 承接库函数返回的布尔值
- `self.pubKeyHash` 在库函数内部会解析为本合约的成员变量（由部署方在替换占位符时绑定）
- `verifyP2PKH` 不会出现在输出 ABI 中——ABI 仅包含合约自身的公共函数 `main`
- 库函数在调用点被内联展开为等价字节码，无额外调用开销
- 产出的 `lock.hex` 为 `76a9<self.pubKeyHash>88ac`，与标准 BTC P2PKH 字节一致

### 8.3 包含结构体的库

```python
Library my.utils:

    Struct Signature:
        r: hex
        s: hex

    def verifyMultiSig(sigs: hex, pubKeys: hex):
        MultiSigVerify(sigs, pubKeys)
```

```python
import my.utils

Contract MultiSigWallet:

    Struct TxInfo:
        amount: int
        recipient: hex

    def main(sigs: hex, pubKeys: hex):
        verifyMultiSig(sigs, pubKeys)
```

**说明：**
- 库可同时导出函数和结构体
- 库结构体与用户合约结构体合并到同一命名空间
- 同名函数或结构体会触发编译期"duplicate"错误

### 8.4 编译命令

```bash
# 设置 stdlib 路径后编译
APC_STDLIB_PATH=$(pwd)/stdlib ./build/bin/utxo_interpreter test/wallet.ct

# 也可通过 user_preferences.json 配置 stdlib 路径
```

产出 `wallet.json`，其中 `abi` 仅包含用户合约公共函数，`lock.hex` 为标准 P2PKH 验证脚本字节码。

---

这些示例严格遵循语法规范和所有权系统，可以作为编写实际业务代码的参考。
