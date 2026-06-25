# How to Write a Contract

---

This section systematically introduces the complete syntax of the UTXO_Compiler contract language. If you prefer to learn from examples, you can start with [Tutorial 1](./tutorials/tutorial-1-p2pkh.md) and come back here as a reference.

---

## 1. Contract Structure

Each `.ct` file can only define **one contract**. The basic skeleton is as follows:

```python
Contract ContractName:
    # Struct definitions (optional, can have multiple)
    Struct StructName:
        fieldName: type

    # Public functions (at least one, serving as the spending entry point)
    def functionName(param: type):
        ...

    # Private helper functions (optional, prefixed with underscore)
    def _helperFunction(param: type):
        ...
```

Contract names, function names, and field names are identifiers following the same rules as Python: starting with a letter or underscore, followed by letters, digits, or underscores.

---

## 2. Data Types

### Primitive Types

| Type           | Description               | Literal Example                            |
| -------------- | ------------------------- | ------------------------------------------ |
| `int`,`number` | Integer (BVM big integer) | `0`, `42`, `-100`                          |
| `string`       | Byte string               | `"hello"`, `"world"`                       |
| `hex`          | Hexadecimal byte array    | `0x1234`, `0xdeadbeef`                     |
| `bool`         | Boolean value             | `1` (true), `0` (false)                    |
| `address`      | Bitcoin P2PKH address     | `"1RainRzqJtJxHTngafpCejDLfYq2y4KBc"`     |

The `address` type only supports standard Base58 P2PKH addresses (starting with `1`, 34 characters). `hex` and `string` are both byte sequences at the underlying level; the difference is only in how the literals are written.

### Fixed-length hex Type

In struct fields, you can use `hexN` to declare fixed-byte-length hexadecimal fields, commonly used to describe fixed-format fields in Bitcoin transactions:

```python
Struct TxInput:
    txid:     hex32   # 32-byte transaction ID
    vout:     hex4    # 4-byte output index
    sequence: hex4    # 4-byte sequence number
```

### Array Types

Struct fields support fixed-length arrays `Type[N]`:

```python
Struct Transaction:
    Inputs:  TxInput[3]    # 3 inputs
    Outputs: TxOutput[3]   # 3 outputs
```

Array subscript access: `tx.Inputs[0]`, where the subscript is usually an integer literal; if the subscript is an integer variable, it is generally used in combination with struct array fields to access the corresponding struct field.

```python
    vout = BinToNum(BVM.unlockingInput.Slice(32, 4)) # Get the position of code in the parent transaction output
    vout_copy = vout.Clone()
    # Get code_data
    code_data = pretx.Outputs[vout_copy].LockingScript.SuffixData.Clone()
```

### `uint64[]` Array

Besides struct arrays, contracts also commonly use `uint64[]` (written as `uint64[N]`) to represent fixed-length 64-bit unsigned integer arrays. When not in use, the array field as a whole occupies one stack position; when needed, it is moved to the top via `OP_ROLL`, then split into individual elements via multiple `OP_SPLIT` operations.

```python
    amount: uint64[6] = temp_data.Slice(3, 48)
    ft_amount_tax = Push(0)
    for i in Range(5, -1, -1):
        ft_amount_tax = BinToNum(amount[i]) + ft_amount_tax
```

Key points:

- The `N` in `uint64[N]` must be a compile-time-determinable fixed length;
- Element access uses `arr[i]`, with indices recommended to traverse fixed boundaries with `Range`;
- `uint64` elements are processed as 8 bytes, suitable for integer sequences like amounts, counters, and indices.

If you need to express a set of count values in a struct, you can also declare directly:

```python
Struct BatchData:
    counts: uint64[4]
```

### Inline Anonymous Struct Types

For compound fields used temporarily, you can inline them directly without defining a separate struct:

```python
utxoData: {txid: hex32, vout: hex4, sequence: hex4}
utxoData = Push(BVM.unlockingInput)
vout = BinToNum(utxoData.vout)
```

---

## 3. Variables

### Assignment

```python
count = 10
result = Hash160(pubKey)     # Bind function return value to a variable
```

Local variables (except arrays and inline anonymous struct types) can be directly assigned and used; struct fields and function parameters must declare types.

### Contract Member Variables (self)

Contract member variables can be used directly; they are replaced with fixed constants in the bytecode at compile time and can be read multiple times in both public and private functions, **not subject to ownership restrictions**:

```python
Contract P2PKH:
    def verify(sig: hex, pubKey: hex):
        pubKey_copy = pubKey.Clone()
        pubKeyHash = Hash160(pubKey_copy)
        EqualVerify(pubKeyHash, self.pubKeyHash)
        result = CheckSig(sig, pubKey)
```

---

## 4. Operators

### Arithmetic Operators

```python
sum   = a + b
diff  = a - b
prod  = a * b
quot  = a / b   # Integer division
```

There is no modulo operator; use the built-in function `Mod(a, b)`.

### Comparison Operators

```python
a == b    # Equal to
a != b    # Not equal to
a <  b    # Less than
a >  b    # Greater than
a <= b    # Less than or equal to
a >= b    # Greater than or equal to
```

Comparison operators return integer `1` (true) or `0` (false), usable directly in `if` conditions or `Return`.

### Logical Operators

The contract language has **no** `and` / `or` / `not` keywords; logical operations are all handled by built-in functions:

```python
ok = And(condition1, condition2)   # Logical AND
ok = Or(condition1, condition2)    # Logical OR
ok = Not(condition)                # Logical NOT
```

---

## 5. Control Flow

### Conditional Statements

```python
if amount > threshold:
    CheckSigVerify(sig, pubKey)
else:
    Return (1 == 0)   # Reject
```

`if` and `else` branches must appear in pairs; multiple branches require nested `if`:

```python
if role == 1:
    _handleBuyer(sig, pubKey)
else:
    if role == 2:
        _handleSeller(sig, pubKey)
    else:
        Return (1 == 0)
```

### Loop Statements

Loops use the `for ... in Range(start, stop, step)` form, with semantics similar to Python's `range`, but the parameter order is `(start, stop_exclusive, step)`:

```python
# Decrement from 2 to 0 (inclusive)
for i in Range(2, -1, -1):
    data = Cat(items[i], data)

# Increment from 0 to 2 (inclusive)
for i in Range(0, 3, 1):
    total = Add(total, values[i])
```

The loop count must be determined at compile time; primarily used to iterate over fixed-size arrays or perform a fixed number of operations.

---

## 6. Functions

### Public Functions (Spending Entry Points)

Functions not starting with `_` are **public functions**. They are exported in the ABI and execute serially in **source declaration order**:

```python
Contract MultiPath:
    # Step 1: verify previous transaction/state
    def loadPreviousState(pretx: PreTX):
        ...

    # Step 2: verify current transaction
    def verifyCurrent(ctx: CurrentTX):
        ...
```

This is different from SDKs where the caller selects one public method. In the current compiler, all public functions form one continuous locking-script flow. For mutually exclusive paths, prefer one public entry with a `path` argument and dispatch internally:

```python
def main(path: int, sig: hex, pubKey: hex):
    if path == 0:
        _spend(sig, pubKey)
    else:
        _refund(sig, pubKey)
```

This is also the pattern used by many contracts under `scrypt_rewrites/` when translating sCrypt examples with multiple public methods.

### Private Helper Functions

Functions starting with `_` can only be called internally within the contract, used to encapsulate repetitive logic:

```python
def _verifyOwner(sig: hex, pubKey: hex, expectedHash: hex):
    pubKeyCopy = pubKey.Clone()
    pubKeyHash = Hash160(pubKeyCopy)
    EqualVerify(pubKeyHash, expectedHash)
    CheckSigVerify(sig, pubKey)

def spend(sig: hex, pubKey: hex, ownerHash: hex):
    ...
    _verifyOwner(sig, pubKey, ownerHash)
    ...
    Return (1 == 1)
```

### Return Statement

`Return` (capitalized) pushes the expression result onto the execution stack and generates an "OP_RETURN":

```python
Return (1 == 1)               # Always pass
Return (1 == 0)               # Always reject
Return CheckSig(sig, pubKey)  # Signature verification result as return value
Return And(cond1, cond2)      # Combined condition
```

Lowercase `return` is used in private functions to return a value to the caller:

```python
def _computeHash(data: hex):
    result = Hash160(data)
    return result
```

---

## 7. Structs

Structs describe the byte layout of composite data, typically corresponding to the format of a segment of transaction data:

```python
Struct Script:
    SuffixData:  string
    PartialHash: string
    Size:        int

Struct Output:
    Value:         int
    LockingScript: Script    # Structs can be nested

Struct PreTX:
    VLIO:                string
    Inputs:              Input[3]
    UnlockingScriptHash: string
    Outputs:             Output[3]
```

Struct field access uses the `.` operator, supporting multi-level chained access:

```python
scriptSize = pretx.Outputs[0].LockingScript.Size
```

> **Note**: Field access consumes the ownership of that field. Accessing the same field twice requires `.Clone()` before the first access. See [Ownership System](./advanced/ownership-system.md) for details.

---

## 8. Destructuring Assignment

The `{}` syntax is used to receive results from functions that return multiple values, or to initialize structs:

```python
# Receive two return values from Split
{header, body} = Split(rawData, 4)

# Receive multiple return values from a private function
{x, y} = _getCoords(encoded)

# Struct literal initialization (in field order)
point: Point = {10, 20}
```

---

## 9. Comments

Use `#` for line comments:

```python
# This is a full-line comment
count: int = 0    # This is an end-of-line comment
```

Multi-line comments are not currently supported; longer comments need `#` on each line.

---

## Next Steps

- [How to Deploy and Call a Contract](./how-to-deploy-and-call-a-contract.md) — Compilation output and on-chain calling process

---

[🇨🇳 中文版](../zh/how-to-write-a-contract.md)
