# Tutorial 1: P2PKH Contract Introduction

---

`P2PKH` (Pay to Public Key Hash) is the most classic and common UTXO locking pattern. This tutorial doesn't start with a "tautological Hello World," but instead begins directly with a real, usable signature contract.

After completing this tutorial, you will be able to:

- Understand the locking conditions and unlocking parameters of `P2PKH`
- Write and compile a complete `p2pkh` contract
- Grasp the necessity of `Clone` in signature verification
- Verify both passing and failing execution paths using the debugger

---

## What Problem Does P2PKH Solve

The goal is simple: only the person who owns a certain private key can spend this UTXO.

The on-chain verification process typically has two steps:

1. Public key hash match: `Hash160(pubKey) == pubKeyHash`
2. Signature valid: `CheckSig(sig, pubKey) == true`

Where `pubKeyHash` is the value fixed into the contract at deployment time (typically the hash of the recipient's address).

---

## Step 1: Write the p2pkh Contract

Create file `p2pkh.ct`:

```python
Contract P2PKH:
    def verify(sig: hex, pubKey: hex):
        pubKey_copy = pubKey.Clone()
        pubKeyHash = Hash160(pubKey_copy)
        EqualVerify(pubKeyHash, self.pubKeyHash)
        result = CheckSig(sig, pubKey)
```

This code corresponds to the standard `P2PKH` logic:

- `verify(sig, pubKey)`: Submit signature and public key when unlocking
- `self.pubKeyHash`: The target public key hash stored in the contract
- `EqualVerify(...)`: Immediately terminate if hashes don't match
- `CheckSig(...)`: Verify whether the signature and public key match

---

## Step 2: Why Clone Is Necessary

The key line is:

```python
pubKeyCopy = pubKey.Clone()
```

Reason: `Hash160(pubKeyCopy)` consumes its input value, and `CheckSig(sig, pubKey)` later still needs the original `pubKey`.
If you don't `Clone` first, the compiler will report "variable has been consumed."

Rules of thumb:

- When the same variable needs to be used by multiple operations, `Clone` before the first consumption
- The last use can directly consume the original variable

---

## Step 3: Compile the Contract

```bash
./utxo_interpreter p2pkh.ct
```

After successful compilation, the JSON output should include:

- Contract name `P2PKH`
- Public function `verify(sig: hex, pubKey: hex)`
- Corresponding bytecode/debug info (depending on compilation parameters)

---

## Step 4: Enter the Debugger to Verify

```bash
./utxo_interpreter p2pkh.ct --debug
```

After running, input parameters (example):

```
Enter parameters for verify:
  sig    [hex]: 0x3045022100...
  pubKey [hex]: 0x03ab12...
```

### Passing Path

Use a public key matching `pubKeyHash` and provide the corresponding signature:

- `EqualVerify` passes
- `CheckSig` returns 1
- Contract ultimately returns true

### Failing Path

You can intentionally input incorrect parameters to test rejection logic:

- Public key doesn't match: terminates directly at `EqualVerify`
- Signature doesn't match: `CheckSig` returns 0, ultimately fails

---

## FAQ

### 1) What format should `pubKeyHash` use?

Use `hex`, typically 20 bytes (the result of `Hash160`).

### 2) Why use `EqualVerify` instead of `Equal`?

`EqualVerify` terminates immediately on failure, better matching the "must satisfy" constraint semantics, and the code is more concise.

### 3) What is the relationship between P2PKH and addresses?

Wallet addresses are typically an encoded representation of `pubKeyHash`. What is actually compared in the contract is the hash value itself, not the address string.

---

## Summary

You have completed the first real, usable UTXO contract, and have mastered the core of `P2PKH`:

- Unlocking function `verify` verifies identity (hash + signature)
- `Clone` resolves ownership/consumption issues
- The debugger can verify both the success and failure paths

---

## Next Steps

- [Tutorial 2: Counter Contract](./tutorial-2-counter.md) — Learn how to design contracts with state-advancing logic

---

[🇨🇳 中文版](../../zh/tutorials/tutorial-1-p2pkh.md)
