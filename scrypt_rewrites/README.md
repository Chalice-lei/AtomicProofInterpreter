# sCrypt Boilerplate Rewrites

This directory contains contracts rewritten from suitable examples in
`sCrypt-Inc/boilerplate/src/contracts`.

Selection rule:
- direct hash, signature, arithmetic, fixed-array, and fixed-loop contracts are included;
- simple current-output constraints, such as `enforceRecipient.ct`, are included when
  they can be represented with explicit fixed output arrays;
- stateful contracts are included when their `this.ctx` output checks can be expressed
  with explicit fixed output arrays that are fully committed to `BVM.outputsHash`;
- HashedMap/HashedSet contracts are not copied directly, but may be rewritten as fixed
  state slots or explicit commitments;
- `scrypt-ord`, Rabin/Merkle/SECP library, and token/ordinal examples are left out unless
  the dependency can be replaced by a compact verifier.

Contracts with multiple sCrypt public methods are rewritten as one public entry point
with a `path` argument, because this compiler treats every non-underscore function as
a public spending entry in source order.
