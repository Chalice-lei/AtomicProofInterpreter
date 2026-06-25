# 语言语法规范

---

本文档描述了当前项目实际实现的语法规则，基于项目的词法分析器、语法分析器和AST结构的实际代码。

---

## 完整语法规范（BNF表示法）

```bnf
// 程序结构
program ::= [import_statement]* [library]* contract

import_statement ::= "import" import_target NEWLINE
import_target ::= dotted_name | STRING
dotted_name ::= IDENTIFIER ["." IDENTIFIER]*

// 库定义
library ::= "Library" dotted_name ":" NEWLINE INDENT [lib_member]+ DEDENT
lib_member ::= function | struct

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
field ::= IDENTIFIER ":" type_expression NEWLINE

type_expression ::= TYPE
                  | TYPE "[" NUMBER "]"      // 定长数组
                  | inline_struct_type        // 内联匿名结构

inline_struct_type ::= "{" field_decl ("," field_decl)* "}"
field_decl ::= IDENTIFIER ":" TYPE

// 语句
statement ::= if_statement
           | for_statement
           | return_statement
           | assignment_statement
           | destructure_assignment
           | variable_declaration
           | expression_statement

block ::= NEWLINE INDENT [statement]* DEDENT

if_statement ::= "if" expression ":" block ["else" ":" block]

for_statement ::= "for" IDENTIFIER "in" "Range" "(" expression "," expression "," expression ")" ":" block

return_statement ::= "Return" expression NEWLINE
                   | "return" expression NEWLINE

assignment_statement ::= (IDENTIFIER | field_access) "=" expression NEWLINE

destructure_assignment ::= "{" identifier_list "}" "=" expression NEWLINE

identifier_list ::= IDENTIFIER ["," IDENTIFIER]*

variable_declaration ::= IDENTIFIER ":" type_expression ["=" expression] NEWLINE

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
literal ::= NUMBER | HEX | STRING | ADDRESS | BOOL

// 操作符
binary_operator ::= "+" | "-" | "*" | "/" 
                 | "==" | "!=" | "<" | ">" | "<=" | ">="

unary_operator ::= "-" | "+"

// 词法元素
IDENTIFIER ::= [a-zA-Z_][a-zA-Z0-9_]*
NUMBER ::= [0-9]+
HEX ::= "0x"[0-9a-fA-F]+
STRING ::= '"' [^"]* '"'
ADDRESS ::= '"' '1' [1-9A-HJ-NP-Za-km-z]{33} '"'   // Base58 P2PKH 地址
TYPE ::= IDENTIFIER ("[" NUMBER "]")?              // 含定长数组形式 hex32 / uint64[6] 等
NEWLINE ::= '\n'
INDENT ::= 增加缩进级别
DEDENT ::= 减少缩进级别
```

---

## 1. 程序结构

### 1.1 程序定义
```
program ::= [import_statement]* [library]* contract
```

一个最终进入语法分析阶段的程序由零个或多个库块，加上一个合约块组成。源码文件可在开头写 `import`，导入器会先把被导入的 `Library` 源码内联展开，然后再交给解析器。

```python
import std.p2pkh
import "./local_utils.ct"

Contract Wallet:
    def main(signature: hex, pubKey: hex):
        ok = verifyP2PKH(signature, pubKey)
        Return ok
```

`import std.p2pkh` 会解析到标准库根目录下的 `std/p2pkh.ct`；`import "./local_utils.ct"` 会按当前文件所在目录解析相对路径。

### 1.2 合约定义
```
contract ::= "Contract" IDENTIFIER ":" NEWLINE INDENT [member]* DEDENT

member ::= function | struct | constructor
```

合约以关键字 `Contract` 开始，后跟合约名称，使用 Python 风格的缩进结构。

### 1.3 库定义

```
library ::= "Library" dotted_name ":" NEWLINE INDENT [lib_member]+ DEDENT
lib_member ::= function | struct
```

库用于复用结构体和辅助函数，名称支持 dotted path，例如 `Library std.p2pkh:`。库块内禁止定义 `__init__` 和 `main`。导入后的库函数会被标记为来自库，不会作为 ABI 的公有函数暴露。

库函数允许引用 `self.X`，这些字段会解析到宿主合约的实例数据。使用这种库时，宿主合约必须按库约定提供对应的 `self` 字段。

---

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

---

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

### 3.2 公有函数与私有函数

- **公有函数**：函数名不以 `_` 开头，是合约的"花费入口"。一个合约可以包含多个公有函数；它们按**源码声明顺序**串行执行，调用方需要为所有公有函数提供解锁参数。
- **私有函数**：函数名以 `_` 开头，仅供合约内部调用，可使用 `return` 把值返回给调用方。
- **库函数**：来自 `Library` 的函数按私有函数处理，不进入 ABI。

```python
def verify(sig: hex, pubKey: hex):       # 公有
    _checkOwner(sig, pubKey)
    Return (1 == 1)

def _checkOwner(sig: hex, pubKey: hex):  # 私有
    EqualVerify(Hash160(pubKey.Clone()), self.pubKeyHash)
    CheckSigVerify(sig, pubKey)
```

### 3.3 构造函数

```
constructor ::= "def" "__init__" "(" [parameter_list] ")" ":" block
```

解析器支持 `__init__`，导出结果也可能包含 `constructorParams`。不过在当前入门工作流中，更常用也更稳定的实例化方式是直接引用 `self.X` 占位符，并在部署阶段替换为真实数据。除非你的上层工具链已经消费 `constructorParams`，否则建议优先使用 `self.X`。

---

## 4. 语句

### 4.1 语句类型
```
statement ::= if_statement
           | for_statement
           | return_statement
           | assignment_statement
           | destructure_assignment
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
if amount > threshold:
    CheckSigVerify(sig, pubKey)
else:
    Return (1 == 0)
```

### 4.3 循环语句
```
for_statement ::= "for" IDENTIFIER "in" "Range" "(" expression "," expression "," expression ")" ":" block
```

`Range(start, stop, step)` 的语义类似 Python 的 `range`，但**循环边界必须在编译期可确定**。`stop` 是开区间。`step` 可正可负，常用 `Range(N-1, -1, -1)` 实现倒序遍历。

**示例：**
```python
# 倒序遍历输出 0..2
for i in Range(2, -1, -1):
    Delete(pretx.Outputs[i].Value)
```

### 4.5 返回语句
```
return_statement ::= "Return" expression NEWLINE
                   | "return" expression NEWLINE
```

`Return`（大写）用于公有函数，将表达式结果作为合约最终返回值；`return`（小写）用于私有函数把值返回给调用方。

### 4.6 赋值语句
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

### 4.7 解构赋值
```
destructure_assignment ::= "{" identifier_list "}" "=" expression NEWLINE
```

用于承接返回多个值的函数（如 `Split`），或按字段顺序初始化结构体字面量。

**示例：**
```python
{header, body} = Split(rawData, 4)        # 多返回值
point: Point = {10, 20}                    # 结构体字面量
```

### 4.8 变量声明
```
variable_declaration ::= IDENTIFIER ":" type_expression ["=" expression] NEWLINE
```

**示例：**
```python
count: int
name: string = "default"
btcAddress: address = "1RainRzqJtJxHTngafpCejDLfYq2y4KBc"
amount: uint64[6] = temp_data.Slice(3, 48)
```

---

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

---

## 6. 词法元素

### 6.1 关键字
```
keywords ::= "Contract" | "Struct" | "def" | "if" | "else" | "for" | "in" | "Return" | "return"
```

> 注意：`Range` 不是关键字，而是只能出现在 `for ... in Range(...)` 头部的内置函数；`Clone` / `Push` / `SetAlt` / `SetMain` / `Delete` / `Keep` 等同样是普通内置函数（详见 [内置函数参考](./builtin-functions.md)）。

### 6.2 标识符和字面量
```
IDENTIFIER ::= [_a-zA-Z][_a-zA-Z0-9]*
NUMBER ::= [0-9]+
HEX ::= "0x"[0-9a-fA-F]+
STRING ::= '"' [^"]* '"'
ADDRESS ::= '"' '1' [1-9A-HJ-NP-Za-km-z]{33} '"'   // Base58 P2PKH 地址（首字符固定为 1）
BOOL ::= "1" | "0"                                  // 与 NUMBER 共享词法形式，由上下文决定语义
TYPE ::= IDENTIFIER ("[" NUMBER "]")?               // 含定长数组形式 hex32 / uint64[6] 等
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

---

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

---

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

---

## 下一步

- [调试器用户使用手册](./debugger_user_manual.md) — 详细的调试器功能说明

---

[🇬🇧 English version](../../en/references/language-specification.md)
