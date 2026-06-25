# How to Deploy and Call a Contract

---

This section covers the complete workflow from compiling a contract to deploying and calling it on the Bitcoin network. UTXO_Compiler handles the compilation phase; on-chain interaction requires a transaction building tool.

---

## Compilation Output

After running the compiler, the output is a JSON-formatted bytecode description:

```bash
./utxo_interpreter my_contract.ct
```

Example output (simplified):

```json
{
  "metadata": {
    ...
  },
  "abi": [
    {
      "type": "function",
      "name": "verify",
      "index": 0,
      "params": [
        {
          "name": "sig",
          "type": "hex"
        },
        {
          "name": "pubKey",
          "type": "hex"
        }
      ]
    }
  ],
  "lock": {
    "asm": "OP_DUP OP_HASH160 <self.pubKeyHash> OP_EQUALVERIFY OP_CHECKSIG",
    "hex": "76a9<self.pubKeyHash>88ac"
  },
  "unlock": {
    "verify": "<sig><pubKey>"
  },
  "functions": [
    {
      "name": "verify",
      "type": "public",
      "params": [
        {
          "name": "sig",
          "type": "hex"
        },
        {
          "name": "pubKey",
          "type": "hex"
        }
      ]
    }
  ]
}
```

Key fields:

- **`lock.hex`**: Hex-encoded bytecode sequence, used directly as the locking script for a transaction output
- **`lock.asm`**: Human-readable opcode disassembly for inspection
- **`functions`**: Function list with their parameters, describing what data must be provided when unlocking

---

## Deploying a Contract

"Deploying a contract" in the UTXO model means: **constructing a transaction whose output's locking script contains the compiled contract bytecode**.

### Deployment Transaction Structure

```
Deployment transaction
├── Input: some existing UTXO (provides funds)
└── Output:
    ├── value:          number of satoshis to lock in the contract
    └── locking script: bytecode compiled by utxo_interpreter
```

### Locking Script and SuffixData

The compiled `lock.hex` typically contains two regions:

- **PartialHash region** — bytecode for all "logic" defined in the contract. Stays the same across deployments.
- **SuffixData region** — trailing bytes that hold instance data such as contract member variables (`self.X`). To deploy different instances of the same contract you only need to swap this region.

After getting `lock.hex`, the deployer must replace each `self.X` placeholder with the real value for this instance. For `P2PKH` that is the 20-byte recipient public key hash (`self.pubKeyHash`); for `Counter` it is the initial count (`self.count`). These instance values land in `SuffixData`, and the contract itself reassembles the full script at runtime via `BVM` and `pretx` to do hash verification (see [Tutorial 2: Counter Contract](./tutorials/tutorial-2-counter.md)).

> UTXO_Compiler does not ship a transaction builder. Splicing `SuffixData` into a real transaction is the job of whichever wallet / tx-building library you use (see "Deployment Example" below).

### Deployment Example (pseudocode)

The pseudocode below sketches a "P2PKH contract deployment". Replace `BitcoinTxBuilder` with whatever Bitcoin transaction library your project uses (bitcoinjs-lib, python-bitcoinlib, bsv, …).

```python
# 1. Compile the contract
$ ./utxo_interpreter p2pkh.ct
# Produces p2pkh.json. lock.hex is a bytecode template with self.pubKeyHash placeholder.

# 2. Substitute self.pubKeyHash with this instance's real hash
lock_hex_template = json["lock"]["hex"]
lock_hex = lock_hex_template.replace(
    "<self.pubKeyHash>",      # placeholder
    "f54a5851e9372b87810a8e60cdd2e7cfd80b6e31"  # actual 20-byte hash
)

# 3. Build a deployment tx that spends an existing UTXO
deploy_tx = BitcoinTxBuilder()
deploy_tx.add_input(utxo=funding_utxo, unlock_script=funding_unlock)
deploy_tx.add_output(
    value=10_000,             # sats locked into the contract
    locking_script=lock_hex,  # compiled output + instance data
)
deploy_tx.add_output(         # change
    value=funding_utxo.value - 10_000 - fee,
    locking_script=change_script,
)

# 4. Broadcast
txid = node.broadcast(deploy_tx.serialize())
print(f"contract UTXO: {txid}:0")
```

Once broadcast, `txid:0` is the deployed contract instance. Any later transaction that wants to spend it must satisfy the unlocking conditions encoded in the contract.

---

## Calling a Contract

"Calling a contract" means **constructing a transaction that spends the contract UTXO**. The caller provides the required data in the input's unlocking script, in order according to the contract's public function parameter list.

### Unlocking Script Structure

The unlocking script is constructed by the caller; its content is the public function's input parameters, pushed onto the stack in order:

```
# Example: P2PKH contract's unlock(sig: hex, pubKey: hex)
Unlocking script = <sig> <pubKey>
```

During BVM execution, the contents of the unlocking script are first pushed onto the main stack (`sig` at the bottom, `pubKey` at the top), then the locking script (contract bytecode) is executed, consuming these parameters from the stack.

### Public Function Execution Order

For contracts containing multiple public functions, the public functions execute **in sequence** in the order they are declared in the source code. The order is determined by the definition position; there is no need to specify "which function to select" at call time.

The caller only needs to provide the unlocking parameters in the agreed order; the execution flow will consume and verify these parameters step by step according to the order of public functions.

### Call Example (pseudocode)

Continuing the P2PKH example, here is a "call transaction" that spends the UTXO created by the deployment tx above:

```python
# 1. Reference the contract UTXO to be spent
contract_utxo = Utxo(txid=deploy_txid, vout=0, value=10_000)

# 2. Prepare unlock parameters in the order of verify(sig: hex, pubKey: hex)
sig    = wallet.sign(spending_tx_preimage, priv_key)
pubKey = wallet.public_key(priv_key)

# 3. Build the unlocking script: push sig first, then pubKey
unlock_script = push_data(sig) + push_data(pubKey)

# 4. Build the spending transaction
spend_tx = BitcoinTxBuilder()
spend_tx.add_input(utxo=contract_utxo, unlock_script=unlock_script)
spend_tx.add_output(value=10_000 - fee, locking_script=recipient_script)

# 5. Broadcast
node.broadcast(spend_tx.serialize())
```

At execution time, the BVM pushes `sig` then `pubKey` onto the main stack, then runs the contract bytecode. After hash and signature checks pass, the top of the stack should be `1`, and the network accepts the transaction.

---

## Notes

A few footguns deployers and callers run into often:

- **`SuffixData` must match exactly what the contract expects.** Even a single byte of padding will make `EqualVerify` inside the contract fail. Drive both deployment and calling from the *same* compilation output.
- **Order and direction of unlock parameters.** The unlocking script's `push` items end up on the main stack in write order, so **the parameter written first sits at the bottom**. Make sure the order matches the public function's parameter list.
- **Multiple public functions execute serially in declaration order.** The caller must supply parameters for *all* public functions, not pick one.
- **`BVM.outputsHash` / `BVM.unlockingInput` etc. are injected by the network at execution time.** When debugging locally, feed simulated transaction data via `settxfile` (see [How to Debug a Contract](./how-to-debug-a-contract.md)).
- **Fees are decided by the caller.** The contract itself does not enforce fees, but miners require a minimum fee based on transaction size before accepting a broadcast.
- **Signature preimage construction.** `CheckSig` validates a preimage computed per the SIGHASH rules — sign with the same SIGHASH type that node validation uses.

---

## Next Steps

- [How to Test a Contract](./how-to-test-a-contract.md) — Local validation before deployment

---

[🇨🇳 中文版](../zh/how-to-deploy-and-call-a-contract.md)
