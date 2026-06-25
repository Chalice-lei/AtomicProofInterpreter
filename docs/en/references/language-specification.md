# Language Grammar Specification

---

This document describes the grammar rules actually implemented in the current project, based on the actual code of the project's lexer, parser, and AST structure.

---

## Complete Grammar Specification (BNF Notation)

```bnf
// Program structure
program ::= [import_statement]* [library]* contract

import_statement ::= "import" import_target NEWLINE
import_target ::= dotted_name | STRING
dotted_name ::= IDENTIFIER ["." IDENTIFIER]*

// Library definition
library ::= "Library" dotted_name ":" NEWLINE INDENT [lib_member]+ DEDENT
lib_member ::= function | struct

// Contract definition
contract ::= "Contract" IDENTIFIER ":" NEWLINE INDENT [member]* DEDENT

member ::= function | struct | constructor

// Function definition
function ::= "def" IDENTIFIER "(" [parameter_list] ")" ["->" return_type] ":" block

constructor ::= "def" "__init__" "(" [parameter_list] ")" ":" block

parameter_list ::= parameter ["," parameter]*
parameter ::= IDENTIFIER ":" TYPE

// Struct definition
struct ::= "Struct" IDENTIFIER ":" NEWLINE INDENT [field]+ DEDENT
field ::= IDENTIFIER ":" type_expression NEWLINE

type_expression ::= TYPE
                  | TYPE "[" NUMBER "]"      // fixed-length array
                  | inline_struct_type        // inline anonymous struct

inline_struct_type ::= "{" field_decl ("," field_decl)* "}"
field_decl ::= IDENTIFIER ":" TYPE

// Statements
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

// Expressions
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

// Literals
literal ::= NUMBER | HEX | STRING | ADDRESS | BOOL

// Operators
binary_operator ::= "+" | "-" | "*" | "/" 
                 | "==" | "!=" | "<" | ">" | "<=" | ">="

unary_operator ::= "-" | "+"

// Lexical elements
IDENTIFIER ::= [a-zA-Z_][a-zA-Z0-9_]*
NUMBER ::= [0-9]+
HEX ::= "0x"[0-9a-fA-F]+
STRING ::= '"' [^"]* '"'
ADDRESS ::= '"' '1' [1-9A-HJ-NP-Za-km-z]{33} '"'   // Base58 P2PKH address
TYPE ::= IDENTIFIER ("[" NUMBER "]")?              // includes fixed-length array forms like hex32 / uint64[6]
NEWLINE ::= '\n'
INDENT ::= increase indentation level
DEDENT ::= decrease indentation level
```

---

## 1. Program Structure

### 1.1 Program Definition
```
program ::= [import_statement]* [library]* contract
```

A source file may start with `import` statements. The import resolver expands imported `Library` sources before parsing, so the final program seen by the parser contains zero or more library blocks followed by exactly one contract.

```python
import std.p2pkh
import "./local_utils.ct"

Contract Wallet:
    def main(signature: hex, pubKey: hex):
        ok = verifyP2PKH(signature, pubKey)
        Return ok
```

`import std.p2pkh` resolves to `std/p2pkh.ct` under the standard-library root. Quoted paths are resolved relative to the importing file.

### 1.2 Contract Definition
```
contract ::= "Contract" IDENTIFIER ":" NEWLINE INDENT [member]* DEDENT

member ::= function | struct | constructor
```

A contract starts with the keyword `Contract`, followed by the contract name, using Python-style indentation.

### 1.3 Library Definition

```
library ::= "Library" dotted_name ":" NEWLINE INDENT [lib_member]+ DEDENT
lib_member ::= function | struct
```

Libraries provide reusable structs and helper functions. Names may use dotted paths, such as `Library std.p2pkh:`. Library blocks may not define `__init__` or `main`. Imported library functions are merged into the host contract and are not exposed as ABI public entry points.

Library functions may refer to `self.X`; these fields resolve to the host contract's instance data. The host contract must provide the fields expected by the library.

---

## 2. Data Structures

### 2.1 Struct Definition
```
struct ::= "Struct" IDENTIFIER ":" NEWLINE INDENT [field]+ DEDENT

field ::= IDENTIFIER ":" TYPE NEWLINE
```

A struct definition must contain at least one field; each field must have a type declaration.

**Example:**
```python
Struct Person:
    name: string
    age: int
    publicKey: hex
    walletAddress: address
```

---

## 3. Function Definitions

### 3.1 Regular Functions
```
function ::= "def" IDENTIFIER "(" [parameter_list] ")" ["->" TYPE] ":" block

parameter_list ::= parameter ["," parameter]*
parameter ::= IDENTIFIER ":" TYPE
```

Function parameters must include type declarations.

**Example:**
```python
def main(sig: string, pubkey: hex):
    result = CheckSig(sig, pubkey)
    return result
```

### 3.2 Public vs. Private Functions

- **Public functions**: name does **not** start with `_`. They are the contract's "spend entry points". A contract may declare multiple public functions; they execute serially in **declaration order**, and the caller must supply unlock parameters for all of them.
- **Private functions**: name starts with `_`. Only callable from inside the contract; may use `return` to send a value back to the caller.
- **Library functions**: functions imported from `Library` blocks are handled like private helpers and are not included in the ABI.

```python
def verify(sig: hex, pubKey: hex):       # public
    _checkOwner(sig, pubKey)
    Return (1 == 1)

def _checkOwner(sig: hex, pubKey: hex):  # private
    EqualVerify(Hash160(pubKey.Clone()), self.pubKeyHash)
    CheckSigVerify(sig, pubKey)
```

### 3.3 Constructor

```
constructor ::= "def" "__init__" "(" [parameter_list] ")" ":" block
```

The parser supports `__init__`, and exported JSON may include `constructorParams`. In the current introductory workflow, however, the more common and stable instantiation pattern is to reference `self.X` placeholders directly and replace them during deployment. Unless your external tooling consumes `constructorParams`, prefer the `self.X` pattern.

---

## 4. Statements

### 4.1 Statement Types
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

### 4.2 Conditional Statements
```
if_statement ::= "if" expression ":" block ["else" ":" block]
```

**Example:**
```python
if amount > threshold:
    CheckSigVerify(sig, pubKey)
else:
    Return (1 == 0)
```

### 4.3 Loop Statements
```
for_statement ::= "for" IDENTIFIER "in" "Range" "(" expression "," expression "," expression ")" ":" block
```

`Range(start, stop, step)` resembles Python's `range`, but **bounds must be known at compile time**. `stop` is exclusive. `step` may be positive or negative; `Range(N-1, -1, -1)` is the common pattern for reverse iteration.

**Example:**
```python
# Iterate output indexes 2..0 in reverse
for i in Range(2, -1, -1):
    Delete(pretx.Outputs[i].Value)
```

### 4.4 Return Statements
```
return_statement ::= "Return" expression NEWLINE
                   | "return" expression NEWLINE
```

`Return` (capital) is used in public functions to deliver the contract's final result; `return` (lowercase) is used in private functions to hand a value back to the caller.

### 4.5 Assignment Statements
```
assignment_statement ::= (IDENTIFIER | field_access) "=" expression NEWLINE

field_access ::= expression "." IDENTIFIER
```

**Example:**
```python
x = 10
self.value = result
Sub.x = 0
```

### 4.6 Destructuring Assignment
```
destructure_assignment ::= "{" identifier_list "}" "=" expression NEWLINE
```

Used to receive multiple return values (e.g. from `Split`) or to initialize a struct literal in field order.

**Example:**
```python
{header, body} = Split(rawData, 4)        # multi-return
point: Point = {10, 20}                    # struct literal
```

### 4.7 Variable Declarations
```
variable_declaration ::= IDENTIFIER ":" type_expression ["=" expression] NEWLINE
```

**Example:**
```python
count: int
name: string = "default"
btcAddress: address = "1RainRzqJtJxHTngafpCejDLfYq2y4KBc"
amount: uint64[6] = temp_data.Slice(3, 48)
```

---

## 5. Expressions

### 5.1 Expression Types
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

**Literal type descriptions:**
- `NUMBER`: Integer literal, e.g., `42`, `-10`
- `HEX`: Hexadecimal literal, e.g., `0xabcd`, `0x1234`
- `STRING`: String literal, e.g., `"hello"`, `"world"`
- `BOOL`: Boolean literal, e.g., `1`, `0`
- `ADDRESS`: Bitcoin address literal; only supports Base58 P2PKH addresses starting with `1`, 34 characters, e.g., `"1RainRzqJtJxHTngafpCejDLfYq2y4KBc"`

**Example:**
```python
# Address literal example
p2pkhAddr: address = "1RainRzqJtJxHTngafpCejDLfYq2y4KBc"  # P2PKH address
```

### 5.2 Function Calls
```
function_call ::= IDENTIFIER "(" [argument_list] ")"

argument_list ::= expression ["," expression]*
```

**Example:**
```python
CheckSig(sig, pubkey)
Sha256(data)
EqualVerify(hash1, hash2)
```

### 5.3 Method Calls and Field Access
```
method_call ::= expression "." IDENTIFIER "(" [argument_list] ")"

field_access ::= expression "." IDENTIFIER
```

Supports chained calls:
```python
obj.field1.field2
obj.field.method()
```

### 5.4 Binary Operations
```
binary_expression ::= expression binary_operator expression

binary_operator ::= "+" | "-" | "*" | "/" 
                 | "==" | "!=" | "<" | ">" | "<=" | ">="
```

### 5.5 Unary Operations
```
unary_expression ::= unary_operator expression

unary_operator ::= "-" | "+"
```

---

## 6. Lexical Elements

### 6.1 Keywords
```
keywords ::= "Contract" | "Struct" | "def" | "if" | "else" | "for" | "in" | "Return" | "return"
```

> Note: `Range` is **not** a keyword — it is a built-in only allowed at the head of `for ... in Range(...)`. `Clone` / `Push` / `SetAlt` / `SetMain` / `Delete` / `Keep` are likewise ordinary built-ins (see [Built-in Functions Reference](./builtin-functions.md)).

### 6.2 Identifiers and Literals
```
IDENTIFIER ::= [_a-zA-Z][_a-zA-Z0-9]*
NUMBER ::= [0-9]+
HEX ::= "0x"[0-9a-fA-F]+
STRING ::= '"' [^"]* '"'
ADDRESS ::= '"' '1' [1-9A-HJ-NP-Za-km-z]{33} '"'   // Base58 P2PKH address (always starts with '1')
BOOL ::= "1" | "0"                                  // shares lexical form with NUMBER; semantics from context
TYPE ::= IDENTIFIER ("[" NUMBER "]")?               // includes fixed-length array forms like hex32 / uint64[6]
```

### 6.3 Operators and Delimiters
```
operators ::= "=" | "==" | "!=" | "<" | ">" | "<=" | ">=" 
           | "+" | "-" | "*" | "/" | "->"

delimiters ::= "(" | ")" | "[" | "]" | ":" | "," | "."
```

### 6.4 Indentation and Newlines
The language uses Python-style indentation to represent code block structure:
- `NEWLINE`: newline character
- `INDENT`: increase indentation level
- `DEDENT`: decrease indentation level

---

## 7. Ownership System

The language implements an ownership concept similar to Rust, but only constrains data variables:

### 7.1 Ownership Rules
- **Variable consumption**: When a local variable is used (as a function argument, assigned to another variable, etc.), the ownership of the original variable is transferred
- **Contract member variable exception**: A contract's member variables (e.g., `self.field`) are directly replaced into the code at compile time, so they are not subject to ownership constraints and can be used multiple times without `Clone`
- **Clone operation**: For local variables, use `Clone()` to create a copy of the variable, avoiding ownership transfer
- **Field access**: Accessing a field of a struct instance consumes the ownership of that field; to access it multiple times, first clone it; but contract member variables are not subject to this restriction
- **Special built-in functions**: `SetAlt()` and `SetMain()` do not consume variable ownership

### 7.2 Clone Syntax
```
clone_expression ::= IDENTIFIER "." "Clone" "(" ")"
```

**Example:**
```python
original_value = 42
cloned_value = original_value.Clone()
# original_value is still usable
```

### 7.3 Built-in Functions
The language provides the following built-in functions:
- **`Clone()`**: Creates a copy of a variable; syntax is `variable.Clone()`
- **`SetAlt(variable)`**: Moves a variable to the alt stack; does not consume variable ownership
- **`SetMain(variable)`**: Moves a variable from the alt stack to the main stack; does not consume variable ownership
- **Other built-in functions**: Such as `CheckSig()`, `Sha256()`, etc.; follow normal function call syntax and consume argument ownership

### 7.4 Ownership System Examples

**Local variable ownership transfer:**
```python
data = Push("0x1234")
result1 = CheckSig(signature, data)  # data is consumed, cannot be used again
# result2 = CheckSig(signature2, data)  # Error: data has been consumed

# Need to clone to use multiple times
data2 = Push("0x5678")
cloned_data = data2.Clone()
result1 = CheckSig(signature1, data2)
result2 = CheckSig(signature2, cloned_data)  # correct: using a cloned copy
```

**Special behavior of contract member variables:**
```python
def main(sig1: string, sig2: string):
    # Contract member variables can be used multiple times without Clone
    result1 = CheckSig(sig1, self.pubKey)
    result2 = CheckSig(sig2, self.pubKey)  # correct: self.pubKey can be used repeatedly
    return {result1, result2}
```

**Special built-in functions:**
```python
data: hex = "0x1234"
SetAlt(data)    # data is still usable; ownership not consumed
SetMain(data)   # data is still usable; ownership not consumed
```

---

## 8. Brace Syntax Sugar ({} Syntax)

### 8.1 Syntax Definition
```
brace_expression ::= "{" [expression_list] "}"
destructure_assignment ::= "{" identifier_list "}" "=" expression NEWLINE
```

### 8.2 Uses and Semantics

The brace syntax sugar `{}` is a multi-purpose syntax feature with different semantics depending on context:

#### 8.2.1 Multiple Return Value Destructuring
Used to receive multiple return values from a function:
```python
{a, b} = Split(data, 10)  # Split returns two values, assigned to a and b respectively
{x, y, z} = _GetCoordinates()  # receive three return values
```

#### 8.2.2 Struct Literals
Used to create struct instances:
```python
Struct Point:
    x: int
    y: int

# Use brace syntax sugar to create a struct instance
p: Point = {10, 20}
```

---

## Next Steps

- [Debugger User Manual](./debugger_user_manual.md) — Detailed debugger feature description

---

[🇨🇳 中文版](../../zh/references/language-specification.md)
