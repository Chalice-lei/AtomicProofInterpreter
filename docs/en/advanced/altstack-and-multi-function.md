# Alt Stack and Multi-Function Collaboration

---

The alt stack is the second stack natively provided by Bitcoin's script execution model (corresponding to `OP_TOALTSTACK` / `OP_FROMALTSTACK`); UTXO_Compiler exposes it as `SetAlt` / `SetMain`. This article focuses on common usage patterns in contract development, especially the high-frequency pattern of passing intermediate state across functions.

---

## Background: Alt Stack Origin and Collaboration Scenarios

In the UTXO model, when spending a contract UTXO, the unlocking script typically calls multiple public functions **in sequence**: earlier functions handle part of the verification, or parse "values needed in later steps" from the parent transaction, locking script, and other contexts; later functions then verify the current transaction's outputs, hash constraints, and so on. These functions are independent definitions in source code, but they **share the same main stack** on the BVM, and there is no parameter passing between functions like in ordinary languages — yet the later step often must depend on intermediate results already computed and verified by the earlier step.

If all such results were passed explicitly again by the unlocking script to the second function, it would be verbose and prone to inconsistencies with actual on-chain data. The more common approach is: as the first function passes its verification, it uses `SetAlt` to push the state that needs to continue into the **alt stack**; the second function then uses `SetMain` to retrieve them in the agreed order and continues the verification.

So the point here is not "inventing a new channel," but using the script's native alt stack for passing intermediate state across functions. The section "Advanced Pattern: Cross-Function State Relay" below demonstrates how the counter contract uses the alt stack to bridge parent transaction verification and current output verification between two-phase functions.

---

## SetAlt and SetMain

### SetAlt

```python
SetAlt(variable)
```

According to the built-in function definition, the main stack and alt stack can be viewed as a continuous linear storage; `SetAlt` moves the specified target variable from the main stack to the alt stack (corresponding to BVM's `OP_TOALTSTACK`), keeping the relative order of remaining elements unchanged.

```python
amount = BinToNum(amountBytes)
SetAlt(amount)     # amount enters the alt stack
# amount can still be used (but usually not recommended, as logically it "has been handed over")
```

### SetMain

```python
SetMain(variable)
```

According to the built-in function definition, `SetMain` adjusts the main/alt stack boundary by specifying the target position in the alt stack (corresponding to BVM's `OP_FROMALTSTACK` semantics), bringing that position and its left elements back to the main stack side.

```python
SetMain(amount)    # retrieve from alt stack, bound to name 'amount'
use(amount)        # use normally
```

> **Last In, First Out (LIFO)**: The alt stack is a standard stack; `SetMain` retrieves the last value pushed in. When pushing and popping multiple values, the order must correspond.

---

## Basic Pattern: Temporary Storage Within a Single Function

The simplest use case is within a single function: temporarily save a value on the alt stack and retrieve it after intermediate operations are complete:

```python
def processWithLoop(items: Data[3]):
    # acc needs to be passed between loop iterations
    acc = Push(0)
    SetAlt(acc)

    for i in Range(2, -1, -1):
        ...
        SetMain(acc)                   # retrieve accumulated value from last iteration
        newAcc = Cat(items[i], acc)    # concatenate new data
        SetAlt(newAcc)                 # push back to alt stack
        ...
    # use acc
    SetMain(acc)                       # loop ends, retrieve final result
```

This pattern is very common when you need to "pass" an accumulating variable through a loop.

---

## Advanced Pattern: Cross-Function State Relay

This is one of the most frequent uses of the alt stack in contract engineering: function A pushes computation results onto the alt stack, and function B retrieves them from the alt stack to continue. The following uses the real logic from `counter.ct` to illustrate.

### Function A: Read Parent Transaction and Write State (`getCountFromPreTX`)

```python
def getCountFromPreTX(pretx: PreTX):
    # ... omit reading related to vout and pretx output ...
    pre_count = BinToNum(code_data.Slice(1, 8))
    SetAlt(pre_count)

    pre_code_partialhash = pretx.Outputs[vout_copy].LockingScript.PartialHash.Clone()
    SetAlt(pre_code_partialhash)
    pre_code_size = pretx.Outputs[vout_copy].LockingScript.Size.Clone()
    SetAlt(pre_code_size)

    # ... omit parent transaction verification logic ...
```

### Function B: Read State and Verify Current Output (`verifyCurrentTX`)

```python
def verifyCurrentTX(ctx: CurrentTX):
    # ... omit some logic in the loop ...
    SetMain(pre_code_size)
    if size == pre_code_size.Clone():
        SetMain(pre_code_partialhash)
        EqualVerify(pre_code_partialhash.Clone(), ctx.Outputs[i].LockingScript.PartialHash.Clone())
        SetMain(pre_count)
        ctx_count = BinToNum(ctx.Outputs[i].LockingScript.SuffixData.Clone().Slice(1, 8))
        EqualVerify(pre_count.Clone() + 1, ctx_count)

        # if still needed later, push back to alt stack
        SetAlt(pre_count)
        SetAlt(pre_code_partialhash)
        SetAlt(pre_code_size)
```

### Ordering Rules

The push and pop order **must be mirror-symmetric**:

```
Push order (SetAlt):  pre_count → pre_code_partialhash → pre_code_size
Pop order (SetMain):  pre_code_size → pre_code_partialhash → pre_count
```

---

## Summary

| Operation | BVM Instruction | Built-in Semantics (brief) | Typical Scenario |
| ------------ | ----------------- | ------------------------------ | -------------------------- |
| `SetAlt(v)` | `OP_TOALTSTACK` | Move target variable from main stack to alt stack | Temporary storage, cross-function state passing |
| `SetMain(v)` | `OP_FROMALTSTACK` | Adjust main/alt stack boundary and restore to main stack side | Retrieve from alt stack, receive cross-function state |

The alt stack is a native Bitcoin script mechanism; in contract engineering, it is commonly used for intermediate state management in complex on-chain logic (multi-step verification, state machines, cross-function protocols).

---

## Next Steps

- [Built-in Functions Reference](../references/builtin-functions.md) — Complete description of all built-in functions

---

[🇨🇳 中文版](../../zh/advanced/altstack-and-multi-function.md)
