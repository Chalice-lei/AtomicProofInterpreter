# Ownership System

---

Ownership is the most unique design in the UTXO_Compiler contract language. It determines the lifetime of variables, directly corresponding to the "pop" semantics of BVM stack values. Understanding these rules is the foundation for writing correct contracts.

---

## Why Ownership Exists

BVM is a stack-based virtual machine: variables correspond to values on the stack, and function calls correspond to pop operations. Most opcodes **consume (pop)** their operands during execution:

```
Stack state: [sig] [pubKey]
Execute OP_CHECKSIG:
  → Pop pubKey and sig
  → Push result (1 or 0)
Stack state: [result]
```

After `OP_CHECKSIG` executes, `pubKey` and `sig` disappear from the stack and no longer exist.

If your contract code tries to use `pubKey` after `CheckSig`, the compiler will report an error immediately.

**The purpose of the ownership system** is to let the compiler catch these errors at compile time rather than waiting for an on-chain execution failure.

---

## Core Rules

### Rule 1: Local Variables Are Consumed After Use

A local variable (a function parameter or a variable declared in the function body) is "consumed" once it appears in any of the following contexts, and cannot be referenced again afterwards:

- As an argument to a built-in function
- Assigned to another variable
- Participating in an arithmetic/comparison operation

```python
def example(data: hex, key: hex):
    hash1 = Hash160(data)     # data is consumed here
    hash2 = Hash160(data)     # ❌ Compile error: data has been consumed
```

### Rule 2: Contract Member Variables Are Not Restricted

`self.fieldName` is replaced with a constant in the bytecode at compile time, taking up no runtime stack position, so it can be read any number of times:

```python
def verify(sig1: hex, sig2: hex):
    r1 = CheckSig(sig1, self.ownerKey)   # self.ownerKey is a compile-time constant
    r2 = CheckSig(sig2, self.ownerKey)   # ✓ Can be used again
```

### Rule 3: Field Access Consumes Field Ownership

After accessing a field of a struct instance, that field is consumed (the struct itself still exists, but that field cannot be accessed again):

```python
def process(output: Output):
    val1 = output.Value          # output.Value is consumed
    val2 = output.Value          # ❌ Error: output.Value has been consumed

    script = output.LockingScript   # Other fields of output are still accessible
```

---

## Clone: Variable Copying

`Clone()` is the standard way to resolve ownership conflicts. It corresponds to BVM's `OP_DUP` instruction:

```python
copy = original.Clone()
```

After execution: `original` is consumed (`OP_DUP` pops the stack top and pushes two copies, but the original "logical position" is consumed), and `copy` is a new independent copy.

### Correct Clone Pattern

```python
# Need to use the same value twice
def example(pubKey: hex):
    forHash = pubKey.Clone()      # Clone a copy for hashing
    h = Hash160(forHash)          # forHash is consumed
    ok = CheckSig(sig, pubKey)    # pubKey is consumed (last use)
```

---

## SetAlt / SetMain: Operations That Don't Consume Ownership

`SetAlt` and `SetMain` are among the few operations in the contract language that **don't consume variable ownership**:

```python
data = Push(0xdeadbeef)
SetAlt(data)     # data moves to alt stack, but the 'data' binding remains valid
SetMain(data)    # retrieve from alt stack, data still valid
use(data)        # still usable
```

These two operations are mainly used for passing state across functions; see [Alt Stack and Multi-Function Collaboration](./altstack-and-multi-function.md) for details.

---

## Delete: Explicit Cleanup

When you're sure a variable is no longer needed, use `Delete` to explicitly destroy it:

```python
{header, body} = Split(rawData, 4)
Delete(header)       # don't need header, clean it up
process(body)
```

`Delete` accepts multiple arguments:

```python
Delete(tmp1, tmp2, tmp3)
```

When used on a struct, it recursively deletes all fields:

```python
Delete(someStruct)   # equivalent to Delete-ing each field one by one
```

---

## Keep: Mark as Retained

`Keep` is a zero-cost marker operation that generates no bytecode; it only tells the compiler "these variables will be used later":

```python
SetAlt(stateA)
SetAlt(stateB)
Keep(stateA, stateB)   # tell compiler not to report "unused variable" warnings
```

---

## Common Errors and Fixes

### Error 1: Function Parameter Passed Multiple Times

```python
def bad(key: hex, sig1: hex, sig2: hex):
    CheckSig(sig1, key)    # key is consumed
    CheckSig(sig2, key)    # ❌ key already consumed
```

Fix:

```python
def good(key: hex, sig1: hex, sig2: hex):
    keyCopy = key.Clone()
    CheckSig(sig1, keyCopy)
    CheckSig(sig2, key)
```

### Error 2: Struct Field Accessed Twice

```python
def bad(output: Output):
    v1 = BinToNum(output.Value)   # output.Value is consumed
    v2 = BinToNum(output.Value)   # ❌
```

Fix:

```python
def good(output: Output):
    rawVal = output.Value.Clone()
    v1 = BinToNum(rawVal)
    v2 = BinToNum(output.Value)
```

### Error 3: Forgetting Destructuring Assignment to Receive Multiple Return Values

```python
Split(data, 10)    # ❌ Split returns two values; must use destructuring assignment to receive
```

Fix:

```python
{left, right} = Split(data, 10)   # ✓
```

### Error 4: Repeatedly Consuming the Same Variable in a Loop

```python
for i in Range(0, 3, 1):
    process(sharedData)    # ❌ sharedData is consumed after the first iteration
```

Fix: clone enough copies of the shared data outside the loop, or use `self` member variables instead.

---

## Next Steps

- [Alt Stack and Multi-Function Collaboration](./altstack-and-multi-function.md) — Complete usage patterns for SetAlt/SetMain

---

[🇨🇳 中文版](../../zh/advanced/ownership-system.md)
