# Built-in Objects Reference

---

The UTXO_Compiler contract language provides two built-in objects: `BVM` and `self`. This document covers the names, purposes, properties, methods, and usage examples for each object, aiming to provide developers with a clear and accurate usage guide.

---

## 1. self Object
- **Object Name**: self
- **Description**: Built-in object representing the current contract instance. `self.X` is replaced at compile time with a constant baked into the bytecode, so it is **not subject to ownership rules** and may be read repeatedly from any public or private function.
- **Example**:
    ```python
    # Use self to access contract member variables
    Contract P2PKH:
        def verify(sig: hex, pubKey: hex):
            pubKey_copy = pubKey.Clone()
            pubKeyHash = Hash160(pubKey_copy)
            EqualVerify(pubKeyHash, self.pubKeyHash)   # check the stored hash for this instance
            CheckSig(sig, pubKey)
    ```

---

## 2. BVM Object
- **Object Name**: BVM (Bytecode Virtual Machine)
- **Description**: A built-in object representing the bytecode virtual machine, providing access to VM metadata such as version, input/output counts.
- **Predefined Member Mapping Table**:
    | Member Name | Corresponding Value | Purpose |
    |-------------|--------------------|-----------| 
    | "version" | 1 | VM version number |
    | "locktime" | 2 | Lock time |
    | "inputCount" | 3 | Number of inputs |
    | "outputCount" | 4 | Number of outputs |
    | "inputsHash" | 5 | Hash of input data |
    | "unlockingInput" | 6 | Unlocking input data |
    | "outputsHash" | 7 | Hash of output data |
- **Example**:
    ```python
    # Process transaction data and verify
    tx_data = Cat(pretx.VLIO, tx_data)  # concatenate transaction data
    txid = Hash256(tx_data)  # compute transaction ID

    # Access BVM object metadata
    meta_data = BVM.unlockingInput  # get unlocking input metadata
    {meta_data_txid, meta_data_remain} = Split(meta_data, 32)  # extract transaction ID from metadata

    EqualVerify(txid, meta_data_txid)  # verify transaction ID consistency
    ```

---

## Next Steps

- [Language Specification](./language-specification.md) — Complete formal grammar definition

---

[🇨🇳 中文版](../../zh/references/builtin-objects.md)
