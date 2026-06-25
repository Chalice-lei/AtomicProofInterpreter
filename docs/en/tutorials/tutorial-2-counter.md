# Tutorial 2: Counter Contract in Practice

---

This tutorial uses the real `counter.ct` contract. It's not a simple "`count + 1` comparison," but a **transaction-level state verification** example:
first extract the old count from the parent transaction, then verify that the new count in the current transaction's output must have incremented.

After completing this tutorial, you will master:

- How to understand `Struct`-driven transaction data modeling
- How to extract old state from `PreTX` and perform parent transaction consistency verification
- How to verify that the count must increment in `CurrentTX`
- How to read `SetMain/SetAlt/Push(BVM.*)` and other low-level stack operations

---

## Contract Goal

The core goal of `counter.ct` is to ensure:

- The current spent input genuinely comes from the declared parent transaction (prevents forging prior state)
- The count value in the corresponding script of the new transaction output must equal the old count + 1
- The current transaction's output hash must match the on-chain `BVM.outputsHash`

---

## Step 1: Understand the Contract Structure

```python
Contract Counter:
    Struct Script:
        SuffixData: string
        PartialHash: string
        Size: number

    Struct Output:
        Value: number
        LockingScript: Script

    Struct Input:
        Data: {txid: hex32, vout: hex4, sequence: hex4}

    Struct PreTX:
        VLIO: string
        Inputs: Input[3]
        UnlockingScriptHash: string
        Outputs: Output[3]

    Struct CurrentTX:
        Outputs: Output[3]

    def getCountFromPreTX(pretx: PreTX):
        utxoData: {txid: hex32, vout: hex4, sequence: hex4}
        utxoData = Push(BVM.unlockingInput)
        vout = BinToNum(utxoData.vout)
        vout_copy = vout.Clone()
        Delete(utxoData.sequence)
        Delete(utxoData.txid)

        code_data = pretx.Outputs[vout_copy].LockingScript.SuffixData.Clone()
        pre_count = BinToNum(code_data.Slice(1, 8))
        SetAlt(pre_count)

        vout_copy = vout.Clone()
        pre_code_partialhash = pretx.Outputs[vout_copy].LockingScript.PartialHash.Clone()
        SetAlt(pre_code_partialhash)
        pre_code_size = pretx.Outputs[vout_copy].LockingScript.Size.Clone()
        SetAlt(pre_code_size)

        # Verify parent transaction
        # Concatenate outputs
        tx_data = Push(0)
        SetAlt(tx_data)
        for i in Range(2, -1, -1):
            size_copy = pretx.Outputs[i].LockingScript.Size.Clone()
            if size_copy != 0:
                tx_data_temp = PartialHash(pretx.Outputs[i].LockingScript.SuffixData, pretx.Outputs[i].LockingScript.PartialHash, pretx.Outputs[i].LockingScript.Size)
                tx_data_temp = Cat(pretx.Outputs[i].Value, tx_data_temp)
                SetMain(tx_data)
                tx_data = Cat(tx_data_temp, tx_data)
                SetAlt(tx_data)
                # Keep(tx_data)
            else:
                Delete(pretx.Outputs[i].LockingScript.Size)
                Delete(pretx.Outputs[i].LockingScript.PartialHash)
                Delete(pretx.Outputs[i].LockingScript.SuffixData)
                Delete(pretx.Outputs[i].Value)

        # Start computing txid
        SetMain(tx_data)
        tx_data = Sha256(tx_data)
        tx_data = Cat(pretx.UnlockingScriptHash, tx_data)
        SetAlt(tx_data)
        # Concatenate inputs
        tx_input_data = Push(0)
        for i in Range(2, -1, -1):
            tx_input_data = Cat(pretx.Inputs[i].Data, tx_input_data)
        tx_input_hash = Sha256(tx_input_data)
        SetMain(tx_data)
        tx_data = Cat(tx_input_hash, tx_data)
        tx_data = Cat(pretx.VLIO, tx_data)
        # TX data concatenation complete, compute txid
        txid = Hash256(tx_data)
        preTXID = BVM.unlockingInput.Slice(0, 32) # Get the txid of the currently unlocking input
        EqualVerify(txid, preTXID)

    def verifyCurrentTX(ctx: CurrentTX):
        time = Push(3)
        SetAlt(time)
        outputs_data = Push(0)
        SetAlt(outputs_data)
        for i in Range(2, -1, -1):
            size = ctx.Outputs[i].LockingScript.Size.Clone()
            size_copy = size.Clone()
            if size_copy != 0:
                SetMain(outputs_data)
                SetMain(time)
                SetMain(pre_code_size)
                pre_code_size_copy = pre_code_size.Clone()
                if size == pre_code_size_copy:
                    SetMain(pre_code_partialhash)
                    pre_code_partialhash_copy = pre_code_partialhash.Clone()
                    code_partialhash = ctx.Outputs[i].LockingScript.PartialHash.Clone()
                    EqualVerify(pre_code_partialhash_copy, code_partialhash)
                    code_suffixdata = ctx.Outputs[i].LockingScript.SuffixData.Clone()
                    ctx_count = BinToNum(code_suffixdata.Slice(1, 8))
                    SetMain(pre_count)
                    pre_count_copy = pre_count.Clone()
                    EqualVerify(pre_count_copy + 1, ctx_count)
                    SetAlt(pre_count)
                    SetAlt(pre_code_partialhash)
                    time = time - 1
                    SetAlt(pre_code_size)
                    SetAlt(time)
                else:
                    SetAlt(pre_code_size)
                    SetAlt(time)
                SetAlt(outputs_data)
                outputs_data_temp = PartialHash(ctx.Outputs[i].LockingScript.SuffixData, ctx.Outputs[i].LockingScript.PartialHash, ctx.Outputs[i].LockingScript.Size)
                outputs_data_temp = Cat(ctx.Outputs[i].Value, outputs_data_temp)
                SetMain(outputs_data)
                outputs_data = Cat(outputs_data_temp, outputs_data)
                SetAlt(outputs_data)
            else:
                Delete(size)
                Delete(ctx.Outputs[i].LockingScript.Size)
                Delete(ctx.Outputs[i].LockingScript.PartialHash)
                Delete(ctx.Outputs[i].LockingScript.SuffixData)
                Delete(ctx.Outputs[i].Value)
        SetMain(outputs_data)
        outputs_data = Sha256(outputs_data)
        EqualVerify(outputs_data, BVM.outputsHash)
        SetMain(time)
        Return (time < 3 == 1)
        Push(self.count)
```

You can think of it as a two-phase process:

1. `getCountFromPreTX(pretx)`: Extract old state from the parent transaction and verify the parent transaction is legitimate
2. `verifyCurrentTX(ctx)`: Verify whether the current transaction output state has correctly advanced

---

## Step 2: Key Function Analysis

### `getCountFromPreTX(pretx)`

This function does three things:

- Uses `Push(BVM.unlockingInput)` to get the `vout` from the current input, locating the output spent in the parent transaction
- Slices out the old count `pre_count` from the `SuffixData` of that output's script
- Re-assembles and hashes the parent transaction data, then `EqualVerify(txid, preTXID)` verifies parent transaction consistency

The significance of this step is: ensuring the "old count" comes from a trusted source, not arbitrary fake data passed by the caller.

### `verifyCurrentTX(ctx)`

This function iterates through the current transaction's outputs and performs state advancement verification:

- For outputs matching the script template, extracts their `ctx_count`
- Verifies `ctx_count == pre_count + 1`
- Aggregates output data and computes `Sha256(outputs_data)`, then compares it with `BVM.outputsHash`
- Finally uses `Return (time < 3 == 1)` to confirm at least one valid advancement branch

---

## Step 3: Compile

```bash
./utxo_interpreter contract_file/counter.ct
```

You should see the `Counter` contract and its public verification functions (specific exported function names depend on compilation output).

---

## Step 4: Debugging and Verification Suggestions

```bash
./utxo_interpreter contract_file/counter.ct --debug
```

### Scenario A: Correct Increment

Construct a `PreTX + CurrentTX` set where the count in the current output is exactly the old count + 1:

- `getCountFromPreTX` passes
- The increment constraint in `verifyCurrentTX` passes
- Output hash matches `BVM.outputsHash`

### Scenario B: Count Jump

If the current count is not the old value + 1 (e.g., directly +3):

- Fails at `EqualVerify(pre_count_copy + 1, ctx_count)`
- Contract rejects the transaction

### Scenario C: Parent Transaction Forgery

If the passed-in `PreTX` cannot reconstruct the correct txid:

- Fails at `EqualVerify(txid, preTXID)`
- Contract rejects the transaction

---

## Extension Suggestions

You can continue extending from this version:

- Generalize `Outputs[3]` / `Inputs[3]` to configurable lengths
- Add a ceiling, time window, or permission signature to count advancement
- Extract transaction concatenation logic into reusable functions to reduce repeated `Cat/Sha256` code

---

## Summary

Through this `Counter` example, you have mastered the core approach for "transaction-level state constraints":

- Use `Struct` to describe input/output transaction data
- First verify parent state, then verify current state advancement
- Use `EqualVerify` to turn key constraints into hard verification points
- Use the debugger to cover both success and rejection paths

---

## Next Steps

- [Ownership System](../advanced/ownership-system.md) — Deep dive into variable consumption and Clone rules

---

[🇨🇳 中文版](../../zh/tutorials/tutorial-2-counter.md)
