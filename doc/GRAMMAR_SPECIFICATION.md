# 类Python脚本语言语法规范

本文档描述了当前项目实际实现的语法规则，基于项目的词法分析器、语法分析器和AST结构的实际代码。

## 完整语法规范（BNF表示法）

```bnf
// 程序结构
program ::= [library]* contract

// 库定义 (可被合约 import 复用的函数 / 结构体集合)
library ::= "Library" library_name ":" NEWLINE INDENT [lib_member]+ DEDENT
library_name ::= IDENTIFIER ["." IDENTIFIER]*
lib_member ::= function | struct    // 禁止 constructor 和 def main

// 合约定义
contract ::= "Contract" IDENTIFIER ":" NEWLINE INDENT [member]* DEDENT

member ::= function | struct | constructor

// 函数定义
function ::= "def" IDENTIFIER "(" [parameter_list] ")" ["->" return_type] ":" block

constructor ::= "def" "__init__" "(" [parameter_list] ")" ":" block

parameter_list ::= parameter ["," parameter]*
parameter ::= IDENTIFIER ":" TYPE

// 结构体定义
struct ::= "Struct" IDENTIFIER ":" NEWLINE INDENT [field]+ DEDENT
field ::= IDENTIFIER ":" TYPE NEWLINE

// 语句
statement ::= if_statement
           | return_statement  
           | assignment_statement
           | destructure_assignment
           | variable_declaration
           | expression_statement

block ::= NEWLINE INDENT [statement]* DEDENT

if_statement ::= "if" expression ":" block ["else" ":" block]

return_statement ::= "Return" expression NEWLINE
                   | "return" expression NEWLINE

assignment_statement ::= (IDENTIFIER | field_access) "=" expression NEWLINE

destructure_assignment ::= "{" identifier_list "}" "=" expression NEWLINE

identifier_list ::= IDENTIFIER ["," IDENTIFIER]*

variable_declaration ::= IDENTIFIER ":" TYPE ["=" expression] NEWLINE

expression_statement ::= expression NEWLINE

// 表达式
expression ::= binary_expression
            | unary_expression
            | primary_expression

binary_expression ::= expression binary_operator expression

unary_expression ::= unary_operator expression

primary_expression ::= IDENTIFIER
                    | literal
                    | function_call
                    | method_call
                    | field_access
                    | Clone_expression
                    | brace_expression
                    | "(" expression ")"

brace_expression ::= "{" [expression_list] "}"

expression_list ::= expression ["," expression]*

function_call ::= IDENTIFIER "(" [argument_list] ")"

method_call ::= expression "." IDENTIFIER "(" [argument_list] ")"

field_access ::= expression "." IDENTIFIER

clone_expression ::= IDENTIFIER "." "Clone" "(" ")"

argument_list ::= expression ["," expression]*

// 字面量
literal ::= NUMBER | HEX | STRING | ADDRESS ｜ BOOL

// 操作符
binary_operator ::= "+" | "-" | "*" | "/" 
                 | "==" | "!=" | "<" | ">" | "<=" | ">="

unary_operator ::= "-" | "+"

// 词法元素
IDENTIFIER ::= [a-zA-Z_][a-zA-Z0-9_]*
NUMBER ::= [0-9]+
HEX ::= "0x"[0-9a-fA-F]+
STRING ::= '"' [^"]* '"'
TYPE ::= IDENTIFIER
NEWLINE ::= '\n'
INDENT ::= 增加缩进级别
DEDENT ::= 减少缩进级别
```

## 1. 程序结构

### 1.1 程序定义
```
program ::= [library]* contract
```

一个程序由**零或多个库 (Library) 块**加上**恰好一个合约 (Contract) 块**组成。库块必须出现在合约之前（通常来自 `import` 展开）。

### 1.2 合约定义
```
contract ::= "Contract" IDENTIFIER ":" NEWLINE INDENT [member]* DEDENT

member ::= function | struct | constructor
```

合约以关键字 `Contract` 开始，后跟合约名称，使用Python风格的缩进结构。

### 1.3 库定义
```
library ::= "Library" library_name ":" NEWLINE INDENT [lib_member]+ DEDENT
library_name ::= IDENTIFIER ["." IDENTIFIER]*
lib_member ::= function | struct
```

库以关键字 `Library` 开始，后跟库名称（支持 dotted path，如 `std.p2pkh`）。库块内只允许 `def` 和 `Struct`，**禁止**构造函数 (`__init__`) 和入口函数 (`main`)。

库函数在编译期被**合并到导入它的用户合约**中，作为可被 `main` 调用的私有函数。它们不出现在输出的 ABI 中。

**库函数对 `self.*` 的引用**：允许。由于库成员在合并步之后与用户合约成员等价，库函数内对 `self.<field>` 的访问会解析为**导入它的合约**的成员变量。这是一种"约定式契约"——使用此类库的合约必须声明对应的成员字段，否则在编译期符号解析阶段报错。例：`std.p2pkh` 内部直接使用 `self.pubKeyHash`，要求宿主合约声明该成员。

**示例：**
```python
# stdlib/std/p2pkh.ct
Library std.p2pkh:

    def verifyP2PKH(signature: hex, pubKey: hex):
        EqualVerify(Hash160(pubKey.Clone()), self.pubKeyHash)
        result = CheckSig(signature, pubKey)
        return result
```

```python
# 用户合约 (必须声明 self.pubKeyHash 成员)
import std.p2pkh

Contract MyWallet:

    def main(signature: hex, pubKey: hex):
        ok = verifyP2PKH(signature, pubKey)
```

## 2. 数据结构

### 2.1 结构体定义
```
struct ::= "Struct" IDENTIFIER ":" NEWLINE INDENT [field]+ DEDENT

field ::= IDENTIFIER ":" TYPE NEWLINE
```

结构体定义必须包含至少一个字段，每个字段都必须有类型声明。

**示例：**
```python
Struct Person:
    name: string
    age: int
    publicKey: hex
    walletAddress: address
```

## 3. 函数定义

### 3.1 普通函数
```
function ::= "def" IDENTIFIER "(" [parameter_list] ")" ["->" TYPE] ":" block

parameter_list ::= parameter ["," parameter]*
parameter ::= IDENTIFIER ":" TYPE
```

函数参数必须包含类型声明。

**示例：**
```python
def main(sig: string, pubkey: hex):
    result = CheckSig(sig, pubkey)
    return result
```

### 3.2 构造函数
```
constructor ::= "def" "__init__" "(" [parameter_list] ")" ":" block
```

构造函数使用特殊名称 `__init__`。

**示例：**
```python
def __init__(pubKeyH: hex):
    self.pubKeyHash = pubKeyH
```

## 4. 语句

### 4.1 语句类型
```
statement ::= if_statement
           | return_statement  
           | assignment_statement
           | variable_declaration
           | expression_statement

block ::= NEWLINE INDENT [statement]* DEDENT
```

### 4.2 条件语句
```
if_statement ::= "if" expression ":" block ["else" ":" block]
```

**示例：**
```python
if _check_lock_time(current_time, timeout):
    CheckSig(senderSig, sender)
else:
    CheckSig(recipientSig, recipient)
```

### 4.3 返回语句
```
return_statement ::= "Return" expression NEWLINE
                   | "return" expression NEWLINE
```

### 4.4 赋值语句
```
assignment_statement ::= (IDENTIFIER | field_access) "=" expression NEWLINE

field_access ::= expression "." IDENTIFIER
```

**示例：**
```python
x = 10
self.value = result
Sub.x = 0
```

### 4.5 变量声明
```
variable_declaration ::= IDENTIFIER ":" TYPE ["=" expression] NEWLINE
```

**示例：**
```python
count: int
name: string = "default"
btcAddress: address = "1RainRzqJtJxHTngafpCejDLfYq2y4KBc"
```

## 5. 表达式

### 5.1 表达式类型
```
expression ::= binary_expression
            | unary_expression
            | primary_expression

primary_expression ::= IDENTIFIER
                    | literal
                    | function_call
                    | method_call
                    | field_access
                    | "(" expression ")"

literal ::= NUMBER | HEX | STRING | ADDRESS
```

**字面量类型说明：**
- `NUMBER`: 整数字面量，如 `42`, `-10`
- `HEX`: 十六进制字面量，如 `0xabcd`, `0x1234`
- `STRING`: 字符串字面量，如 `"hello"`, `"世界"`
- `BOOL`: 布尔字面量，如`1`, `0`
- `ADDRESS`: 比特币地址字面量，仅支持以 `1` 开头的Base58地址（P2PKH），34字符，如 `"1RainRzqJtJxHTngafpCejDLfYq2y4KBc"`

**示例：**
```python
# 地址字面量示例
p2pkhAddr: address = "1RainRzqJtJxHTngafpCejDLfYq2y4KBc"  # P2PKH地址
```

### 5.2 函数调用
```
function_call ::= IDENTIFIER "(" [argument_list] ")"

argument_list ::= expression ["," expression]*
```

**示例：**
```python
CheckSig(sig, pubkey)
Sha256(data)
EqualVerify(hash1, hash2)
```

### 5.3 方法调用和字段访问
```
method_call ::= expression "." IDENTIFIER "(" [argument_list] ")"

field_access ::= expression "." IDENTIFIER
```

支持链式调用：
```python
obj.field1.field2
obj.field.method()
```

### 5.4 二元运算
```
binary_expression ::= expression binary_operator expression

binary_operator ::= "+" | "-" | "*" | "/" 
                 | "==" | "!=" | "<" | ">" | "<=" | ">="
```

### 5.5 一元运算
```
unary_expression ::= unary_operator expression

unary_operator ::= "-" | "+"
```

## 6. 词法元素

### 6.1 关键字
```
keywords ::= "Contract" | "def" | "Struct" | "if" | "else" | "Return" | "return"
```

### 6.2 标识符和字面量
```
IDENTIFIER ::= [_a-zA-Z][_a-zA-Z0-9]*
NUMBER ::= [0-9]+
HEX ::= "0x"[0-9a-fA-F]+
STRING ::= '"' [^"]* '"'
ADDRESS ::= '"' '1' [0-9a-fA-F]{33} '"'
TYPE ::= IDENTIFIER  // 类型标识符，如 int, string, hex, bool, address
```

### 6.3 操作符和分隔符
```
operators ::= "=" | "==" | "!=" | "<" | ">" | "<=" | ">=" 
           | "+" | "-" | "*" | "/" | "->"

delimiters ::= "(" | ")" | "[" | "]" | ":" | "," | "."
```

### 6.4 缩进和换行
语言使用Python风格的缩进来表示代码块结构：
- `NEWLINE`: 换行符
- `INDENT`: 增加缩进级别
- `DEDENT`: 减少缩进级别

## 7. 所有权系统

该语言实现了类似Rust的所有权概念，但仅对数据变量有约束：

### 7.1 所有权规则
- **变量消耗**: 当局部变量被使用（作为函数参数、赋值给其他变量等）时，原变量的所有权被转移
- **合约成员变量例外**: 合约的成员变量（如 `self.field`）在编译时会被直接替换到代码中，因此不受所有权约束，可以多次使用而无需 Clone
- **克隆操作**: 对于局部变量，使用 `Clone()` 函数可以创建变量的副本，避免所有权转移
- **字段访问**: 访问结构体实例的字段会消耗该字段的所有权，如需多次访问需要先克隆；但合约成员变量不受此限制
- **特殊内置函数**: `SetAlt()` 和 `SetMain()` 函数不会消耗变量的所有权

### 7.2 克隆语法
```
clone_expression ::= IDENTIFIER "." "Clone" "(" ")"
```

**示例：**
```python
original_value = 42
cloned_value = original_value.Clone()
# original_value 仍然可用
```

### 7.3 内置函数
语言提供以下内置函数：
- **`Clone()`**: 创建变量的副本，语法为 `variable.Clone()`
- **`SetAlt(variable)`**: 将变量移动到副栈，不消耗变量所有权
- **`SetMain(variable)`**: 将变量从副栈移动到主栈，不消耗变量所有权
- **其他内置函数**: 如 `CheckSig()`, `Sha256()` 等，遵循普通函数调用语法并消耗参数的所有权

### 7.4 所有权系统示例

**局部变量的所有权转移：**
```python
data = Push("0x1234")
result1 = CheckSig(signature, data)  # data 被消耗，不能再次使用
# result2 = CheckSig(signature2, data)  # 错误：data 已被消耗

# 需要克隆才能多次使用
data2 = Push("0x5678")
cloned_data = data2.Clone()
result1 = CheckSig(signature1, data2)
result2 = CheckSig(signature2, cloned_data)  # 正确：使用克隆的副本
```

**合约成员变量的特殊行为：**
```python
def main(sig1: string, sig2: string):
    # 合约成员变量可以多次使用，无需 Clone
    result1 = CheckSig(sig1, self.pubKey)
    result2 = CheckSig(sig2, self.pubKey)  # 正确：self.pubKey 可以重复使用
    return {result1, result2}
```

**特殊内置函数：**
```python
data: hex = "0x1234"
SetAlt(data)    # data 仍然可用，所有权未被消耗
SetMain(data)   # data 仍然可用，所有权未被消耗
```

## 8. 大括号语法糖 ({} 语法)

### 8.1 语法定义
```
brace_expression ::= "{" [expression_list] "}"
destructure_assignment ::= "{" identifier_list "}" "=" expression NEWLINE
```

### 8.2 用途和语义

大括号语法糖 `{}` 是一个多功能的语法特性，根据上下文有不同的语义：

#### 8.2.1 多返回值解构
用于承接函数的多个返回值：
```python
{a, b} = Split(data, 10)  # Split 返回两个值，分别赋给 a 和 b
{x, y, z} = _GetCoordinates()  # 承接三个返回值
```

#### 8.2.2 结构体字面量
用于创建结构体实例：
```python
Struct Point:
    x: int
    y: int

# 使用大括号语法糖创建结构体实例
p: Point = {10, 20}
```
