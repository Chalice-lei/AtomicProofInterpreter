# Bitcoin Basics

---

This section covers the fundamental Bitcoin concepts you need to write UTXO contracts, including how the UTXO model works, the execution mechanism of Bitcoin Script, and the role UTXO_Compiler plays in all of this. Readers already familiar with these topics can skip directly to [How to Write a Contract](./how-to-write-a-contract.md).

---

## Bitcoin Transaction Structure

A Bitcoin transaction contains several **inputs** and **outputs**.

### Output

Each output contains two parts:

- **value**: how many satoshis this output locks (1 BTC = 10⁸ satoshis)
- **locking script**: a piece of bytecode describing "who can spend this money, and under what conditions"

Once an output is created, its value and locking script are fixed until someone spends it. Unspent outputs are called **UTXOs (Unspent Transaction Outputs)**.

### Input

Each input contains:

- **Reference to a previous output**: the txid of the previous transaction + the output index (vout) within that transaction
- **unlocking script**: provides the "key" — the data needed to satisfy the conditions set by the locking script

### Execution Model

When a miner validates a transaction, for each input it **concatenates and executes** the unlocking script followed by the corresponding locking script. Only if the value at the top of the main stack is true (non-zero) at the end is the input valid and the spend allowed.

```
Execution order:
[Unlocking script opcode sequence]  →  [Locking script opcode sequence]
         ↓
       BVM main stack
         ↓
     Stack top = true?  →  ✓ Spend valid
     Stack top = false? →  ✗ Spend rejected
```

---

## BVM Stack Mechanism

BVM (Bitcoin Virtual Machine) is a **stack-based** state machine with no registers and no heap — only two stacks:

- **Main stack**: most operations happen on the main stack
- **Alt stack**: temporary storage area; data is exchanged with the main stack via `OP_TOALTSTACK` / `OP_FROMALTSTACK`

Each opcode either pops some values from the stack top as input, pushes results to the stack top, or both.

### A Simple Example

Executing `<pubKeyHash> OP_EQUAL`:

```
Initial stack (pushed by unlocking script): [computedHash]
Execute OP_EQUAL:              Pop computedHash and pubKeyHash, push comparison result
Final stack:                   [1]   ← true, verification passed
```

### Why Ownership Relates to the Stack Model

In BVM, each value on the stack exists only once. Opcode `OP_DUP` duplicates the top value (corresponding to `.Clone()` in the contract language), and most opcodes **consume** (pop) their operands after execution.

This directly explains the contract language's ownership rules: variables correspond to stack values, passing to a function = popping, usable only once; `.Clone()` = `OP_DUP`, copy first then use.

---

## Types of Locking Scripts

### P2PKH (Pay to Public Key Hash)

The most common form of Bitcoin payment. Locking script format:

```
OP_DUP OP_HASH160 <pubKeyHash> OP_EQUALVERIFY OP_CHECKSIG
```

Verification logic:
1. Duplicate the public key provided by the spender
2. Compute Hash160 of the public key
3. Compare with the stored pubKeyHash — must be equal
4. Verify the signature with the public key

Corresponding UTXO_Compiler contract:

```python
Contract P2PKH:
    # Pay-to-Public-Key-Hash verification contract
    # Verifies whether the transaction signature matches the public key hash

    def verify(sig: hex, pubKey: hex):
        # Verification function: check signature and public key
        # Parameters: sig: transaction signature  pubKey: public key
        pubKey_copy = pubKey.Clone()
        
        # Compute the hash of the public key
        pubKeyHash = Hash160(pubKey_copy)
        
        # Verify the public key hash matches
        # Uses member variable self.pubKeyHash
        EqualVerify(pubKeyHash, self.pubKeyHash)
        
        # Verify the signature
        result = CheckSig(sig, pubKey)
```

### P2SH (Pay to Script Hash)

The locking script stores only the hash of the script. When spending, the spender provides the original script and corresponding unlocking data. UTXO_Compiler does not directly generate P2SH wrappers, but the compiled bytecode can be used as a redeem script.

### Custom Contract Scripts

This is the core use case for UTXO_Compiler: writing locking scripts with arbitrary custom logic to implement on-chain protocols such as multi-sig, timelocks, hash locks, collateralized lending, and more.

---

## How Contract State Exists in the UTXO Model

Ethereum contracts have "on-chain storage" — a key-value table that contract functions can read and write. The UTXO model has **no** such mechanism. So where does contract state live?

The answer is: **state is compiled into the bytecode itself** and moves with the UTXO.

In UTXO_Compiler, contract member variables are directly embedded in the compiled bytecode. Different data provided by users produces different locking scripts, i.e., different "contract instances."

```
Contract source (template)
     │
     │ Instantiation (user provides data)
     ▼
Locking script bytecode (containing fixed state values)
     │
     │ Embedded in transaction output
     ▼
   UTXO (exists on-chain)
```

If a contract needs to update its state after execution (e.g., transferring collateral after a lending contract settles), the new state is expressed by creating a **new UTXO** — the new UTXO's locking script contains the updated state bytecode. This pattern of "state moves with assets" is the essence of UTXO contract design and the origin of the `SuffixData` mechanism.

---

## Transaction Verification and the pretx Parameter

UTXO_Compiler contracts often need to verify the **parent transaction** (the transaction that is the input source of the transaction currently spending this UTXO), to prevent the spender from forging parameters.

During BVM execution, `BVM.unlockingInput` (the raw data of the current input, including the txid reference) is available. A contract can:

1. Receive the parent transaction data passed by the spender (`pretx` parameter)
2. Manually compute the txid of the parent transaction
3. Compare it with the txid recorded in `BVM.unlockingInput`
4. If they match, the parent transaction data is authentic

This verification pattern is demonstrated in detail in the [Advanced Tutorial](./tutorials/tutorial-2-counter.md).

---

## Summary

| Concept | Meaning | Corresponding in contract language |
|---------|---------|-----------------------------------|
| UTXO | Unspent transaction output carrying value and locking script | A `.ct` contract instance |
| Locking script | Bytecode description of spending conditions | Compilation output |
| Unlocking script | Data and proofs provided when spending | Call arguments to public functions |
| BVM | Stack-based virtual machine executing scripts | Underlying basis for the ownership system |
| Main stack | BVM's main workspace | Local variables |
| Alt stack | BVM's temporary storage area | `SetAlt` / `SetMain` |
| OP_DUP | Duplicate top stack value | `.Clone()` |

---

## Next Steps

- [How to Write a Contract](./how-to-write-a-contract.md) — Learn the complete contract language syntax

---

[🇨🇳 中文版](../zh/bitcoin-basics.md)
