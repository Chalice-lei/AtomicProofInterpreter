# Built-in Functions Reference

---

This document lists all built-in functions of the UTXO_Compiler contract language, including function descriptions, parameter explanations, return values, exception scenarios, and usage examples. It covers multiple categories: cryptography, signature verification, arithmetic operations, comparison operations, stack operations (main stack and alt stack), data cloning, slicing, deletion, and pushing. The goal is to provide developers with a clear and accurate function usage guide.

---

## I. Cryptographic Functions

### 1. Rmd160
- **Function Name**: Rmd160
- **Description**: Computes the RIPEMD160 hash of the input byte array.
- **Parameters**:
  - `input_data`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: Raw data to be hashed
    - Default: None
- **Returns**:
  - Type: BYTES (fixed length of 20 bytes)
  - Meaning: RIPEMD160 hash result of the input data
- **Example**:
  ```python
  input_bytes: hex = 0x1234567890
  result = Rmd160(input_bytes)
  ```

### 2. Sha1
- **Function Name**: Sha1
- **Description**: Computes the SHA1 hash of the input byte array.
- **Parameters**:
  - `input_data`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: Raw data to be hashed
    - Default: None
- **Returns**:
  - Type: BYTES (fixed length of 20 bytes)
  - Meaning: SHA1 hash result of the input data
- **Example**:
  ```python
  input_bytes: hex = 0x1234567890
  result = Sha1(input_bytes)
  ```

### 3. Sha256
- **Function Name**: Sha256
- **Description**: Computes the SHA256 hash of the input byte array.
- **Parameters**:
  - `input_data`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: Raw data to be hashed
    - Default: None
- **Returns**:
  - Type: BYTES (fixed length of 32 bytes)
  - Meaning: SHA256 hash result of the input data
- **Example**:
  ```python
  input_bytes: hex = 0x1234567890
  result = Sha256(input_bytes)
  ```

### 4. Hash160
- **Function Name**: Hash160
- **Description**: Performs a combined hash computation on the input byte array: SHA256 followed by RIPEMD160.
- **Parameters**:
  - `input_data`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: Raw data for the combined hash computation
    - Default: None
- **Returns**:
  - Type: BYTES (fixed length of 20 bytes)
  - Meaning: Hash result of the input data after SHA256+RIPEMD160
- **Example**:
  ```python
  input_bytes: hex = 0x1234567890
  result = Hash160(input_bytes)
  ```

### 5. Hash256
- **Function Name**: Hash256
- **Description**: Performs double SHA256 hash computation on the input byte array.
- **Parameters**:
  - `input_data`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: Raw data for double SHA256
    - Default: None
- **Returns**:
  - Type: BYTES (fixed length of 32 bytes)
  - Meaning: Hash result of the input data after two SHA256 computations
- **Example**:
  ```python
  input_bytes: hex = 0x1234567890
  result = Hash256(input_bytes)
  ```

### 6. PartialHash
- **Function Name**: PartialHash
- **Description**: Performs a partial hash combination computation on three input byte arrays.
- **Parameters**:
  - `part1`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: First part of data for hash computation
    - Default: None
  - `part2`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: Second part of data for hash computation
    - Default: None
  - `part3`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: Third part of data for hash computation
    - Default: None
- **Returns**:
  - Type: BYTES
  - Meaning: Partial hash result of the three combined input parts
- **Example**:
  ```python
  p1: hex = 0x1234
  p2: hex = 0x5678
  p3: hex = 0x9012
  result = PartialHash(p1, p2, p3)
  ```

---

## II. Signature Verification Functions

### 1. CheckSig
- **Function Name**: CheckSig
- **Description**: Verifies whether a signature matches a public key; returns a boolean result.
- **Parameters**:
  - `signature`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: Signature data to be verified
    - Default: None
  - `public_key`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: Public key used to verify the signature
    - Default: None
- **Returns**:
  - Type: BOOLEAN
  - Meaning: `1` indicates the signature is valid; `0` indicates invalid
- **Example**:
  ```python
  sig: hex = 0x1234       # assumed signature byte array
  pub_key: hex = 0x5678   # corresponding public key
  result = CheckSig(sig, pub_key)
  ```

### 2. CheckSigVerify
- **Function Name**: CheckSigVerify
- **Description**: Verifies whether a signature matches a public key; terminates execution if verification fails.
- **Parameters**:
  - `signature`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: Signature data to be verified
    - Default: None
  - `public_key`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: Public key used to verify the signature
    - Default: None
- **Returns**: None
- **Example**:
  ```python
  # Verify valid signature (normal execution)
  valid_sig: hex = 0x1234
  pub_key: hex = 0x5678
  CheckSigVerify(valid_sig, pub_key)  # no output, execution continues

  # Verify invalid signature (throws exception and terminates)
  invalid_sig: hex = 0x9012
  CheckSigVerify(invalid_sig, pub_key)  # execution terminates
  ```

### 3. MultiSig
- **Function Name**: MultiSig
- **Description**: Verifies whether a set of multiple signatures matches a set of public keys (typically requires "at least m out of n signatures valid" logic); returns a boolean result.
- **Parameters**:
  - `signatures`
    - Type: BYTES (serialized data containing multiple signatures)
    - Optional/Required: Required
    - Purpose: Collection of multiple signatures to be verified
    - Default: None
  - `public_keys`
    - Type: BYTES (serialized data containing multiple public keys)
    - Optional/Required: Required
    - Purpose: Collection of public keys for verification
    - Default: None
- **Returns**:
  - Type: BOOLEAN
  - Meaning: `1` indicates multi-signature verification passed; `0` indicates failed
- **Example**:
  ```python
  sigs: hex = 0x1234567890      # serialized valid signature set
  pub_keys: hex = 0x0987654321  # serialized public key set
  result = MultiSig(sigs, pub_keys)
  ```

### 4. MultiSigVerify
- **Function Name**: MultiSigVerify
- **Description**: Verifies whether a set of multiple signatures matches a set of public keys; terminates execution if verification fails.
- **Parameters**:
  - `signatures`
    - Type: BYTES (serialized data containing multiple signatures)
    - Optional/Required: Required
    - Purpose: Collection of multiple signatures to be verified
    - Default: None
  - `public_keys`
    - Type: BYTES (serialized data containing multiple public keys)
    - Optional/Required: Required
    - Purpose: Collection of public keys for verification
    - Default: None
- **Returns**: None
- **Example**:
  ```python
  # Verify valid multi-signature (normal execution)
  valid_sigs: hex = 0x1234567890
  pub_keys: hex = 0x0987654321
  MultiSigVerify(valid_sigs, pub_keys)  # no output, execution continues

  # Verify invalid multi-signature (throws exception and terminates)
  invalid_sigs: hex = 0x6789012345
  MultiSigVerify(invalid_sigs, pub_keys)  # execution terminates
  ```

---

## III. Unary Arithmetic Functions

### 1. Inc
- **Function Name**: Inc
- **Description**: Adds 1 to the input integer and returns the result.
- **Parameters**:
  - `x`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Integer to increment
    - Default: None
- **Returns**:
  - Type: INTEGER
  - Meaning: Input integer plus 1
- **Example**:
  ```python
  a: int = 5
  result = Inc(a)   # result: 6
  ```

### 2. Dec
- **Function Name**: Dec
- **Description**: Subtracts 1 from the input integer and returns the result.
- **Parameters**:
  - `x`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Integer to decrement
    - Default: None
- **Returns**:
  - Type: INTEGER
  - Meaning: Input integer minus 1
- **Example**:
  ```python
  a: int = 5
  result = Dec(a)   # result: 4
  ```

### 3. Neg
- **Function Name**: Neg
- **Description**: Negates the input integer and returns the result.
- **Parameters**:
  - `x`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Integer to negate
    - Default: None
- **Returns**:
  - Type: INTEGER
  - Meaning: The negation of the input integer
- **Example**:
  ```python
  a: int = 5
  result = Neg(a)   # result: -5
  ```

### 4. Abs
- **Function Name**: Abs
- **Description**: Returns the absolute value of the input integer.
- **Parameters**:
  - `x`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Integer to take absolute value of
    - Default: None
- **Returns**:
  - Type: INTEGER
  - Meaning: Absolute value of the input integer
- **Example**:
  ```python
  a: int = -5
  result = Abs(a)   # result: 5
  ```

### 5. Not
- **Function Name**: Not
- **Description**: Performs a logical NOT on the input integer; returns a boolean value.
- **Parameters**:
  - `x`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Integer to perform logical NOT on
    - Default: None
- **Returns**:
  - Type: BOOLEAN
  - Meaning: Returns `1` if input is 0; otherwise returns `0`
- **Example**:
  ```python
  a: int = 5
  result = Not(a)   # result: 0
  ```

### 6. ZeroNotEqual
- **Function Name**: ZeroNotEqual
- **Description**: Checks whether the input integer is not equal to 0; returns a boolean value.
- **Parameters**:
  - `x`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Integer to check
    - Default: None
- **Returns**:
  - Type: BOOLEAN
  - Meaning: Returns `1` if input is non-zero; returns `0` if input is 0
- **Example**:
  ```python
  a: int = 0
  result = ZeroNotEqual(a)   # result: 0
  ```

---

## IV. Binary Arithmetic Functions

### 1. Add
- **Function Name**: Add
- **Description**: Computes the sum of two integers and returns the result.
- **Parameters**:
  - `a`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: First addend
    - Default: None
  - `b`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Second addend
    - Default: None
- **Returns**:
  - Type: INTEGER
  - Meaning: Sum of the two input integers
- **Example**:
  ```python
  a: int = 2
  b: int = 3
  result = Add(a, b)   # result: 5
  ```

### 2. Sub
- **Function Name**: Sub
- **Description**: Computes the difference of the first integer minus the second integer and returns the result.
- **Parameters**:
  - `a`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Minuend
    - Default: None
  - `b`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Subtrahend
    - Default: None
- **Returns**:
  - Type: INTEGER
  - Meaning: Result of `a - b`
- **Example**:
  ```python
  a: int = 5
  b: int = 3
  result = Sub(a, b)   # result: 2
  ```

### 3. Mul
- **Function Name**: Mul
- **Description**: Computes the product of two integers and returns the result.
- **Parameters**:
  - `a`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: First multiplicand
    - Default: None
  - `b`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Second multiplicand
    - Default: None
- **Returns**:
  - Type: INTEGER
  - Meaning: Product of the two input integers
- **Example**:
  ```python
  a: int = 2
  b: int = 3
  result = Mul(a, b)   # result: 6
  ```

### 4. Div
- **Function Name**: Div
- **Description**: Computes the quotient of the first integer divided by the second integer and returns the result.
- **Parameters**:
  - `a`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Dividend
    - Default: None
  - `b`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Divisor (must not be 0)
    - Default: None
- **Returns**:
  - Type: INTEGER
  - Meaning: Integer quotient of `a` divided by `b`
- **Example**:
  ```python
  a: int = 10
  b: int = 2
  result = Div(a, b)   # result: 5
  ```

### 5. Mod
- **Function Name**: Mod
- **Description**: Computes the remainder of the first integer divided by the second integer and returns the result.
- **Parameters**:
  - `a`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Dividend
    - Default: None
  - `b`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Divisor (must not be 0)
    - Default: None
- **Returns**:
  - Type: INTEGER
  - Meaning: Remainder of `a` divided by `b`
- **Example**:
  ```python
  a: int = 10
  b: int = 3
  result = Mod(a, b)   # result: 1
  ```

---

## V. Shift Functions

### 1. Lshift
- **Function Name**: Lshift
- **Description**: Left-shifts the first integer by the number of bits specified by the second integer and returns the result.
- **Parameters**:
  - `a`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Integer to left-shift
    - Default: None
  - `b`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Number of bits to shift (must be ≥0)
    - Default: None
- **Returns**:
  - Type: INTEGER
  - Meaning: Result of `a` left-shifted by `b` bits
- **Example**:
  ```python
  a: int = 4
  b: int = 2
  result = Lshift(a, b)   # result: 16
  ```

### 2. Rshift
- **Function Name**: Rshift
- **Description**: Right-shifts the first integer by the number of bits specified by the second integer and returns the result.
- **Parameters**:
  - `a`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Integer to right-shift
    - Default: None
  - `b`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Number of bits to shift (must be ≥0)
    - Default: None
- **Returns**:
  - Type: INTEGER
  - Meaning: Result of `a` right-shifted by `b` bits
- **Example**:
  ```python
  a: int = 8
  b: int = 3
  result = Rshift(a, b)   # result: 1
  ```

---

## VI. Non-numeric Comparison Functions

### 1. Equal
- **Function Name**: Equal
- **Description**: Checks whether two byte arrays are equal; returns a boolean value.
- **Parameters**:
  - `bytes1`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: First byte array to compare
    - Default: None
  - `bytes2`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: Second byte array to compare
    - Default: None
- **Returns**:
  - Type: BOOLEAN
  - Meaning: Returns `1` if both byte arrays are identical; otherwise returns `0`
- **Example**:
  ```python
  a: string = "hello"
  b: string = "hello"
  result = Equal(a, b)   # result: 1
  ```

### 2. EqualVerify
- **Function Name**: EqualVerify
- **Description**: Checks whether two byte arrays are equal; terminates execution if they are not equal.
- **Parameters**:
  - `bytes1`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: First byte array to compare
    - Default: None
  - `bytes2`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: Second byte array to compare
    - Default: None
- **Returns**: None
- **Example**:
  ```python
  # Equal case (normal execution)
  a: string = "hello"
  b: string = "hello"
  EqualVerify(a, b)  # no output, execution continues

  # Not equal case (throws exception and terminates)
  a: string = "hello"
  b: string = "world"
  EqualVerify(a, b)  # execution terminates
  ```

---

## VII. Numeric Comparison Functions

### 1. NumEqual
- **Function Name**: NumEqual
- **Description**: Checks whether two integers are equal; returns a boolean value.
- **Parameters**:
  - `num1`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: First integer to compare
    - Default: None
  - `num2`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Second integer to compare
    - Default: None
- **Returns**:
  - Type: BOOLEAN
  - Meaning: Returns `1` if the two integers are equal; otherwise returns `0`
- **Example**:
  ```python
  a: int = 5
  b: int = 5
  result = NumEqual(a, b)   # result: 1
  ```

### 2. NumEqualVerify
- **Function Name**: NumEqualVerify
- **Description**: Checks whether two integers are equal; terminates execution if they are not equal.
- **Parameters**:
  - `num1`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: First integer to compare
    - Default: None
  - `num2`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Second integer to compare
    - Default: None
- **Returns**: None
- **Example**:
  ```python
  # Equal case (normal execution)
  a: int = 5
  b: int = 5
  NumEqualVerify(a, b)  # no output, execution continues

  # Not equal case (throws exception and terminates)
  a: int = 5
  b: int = 3
  NumEqualVerify(a, b)  # execution terminates
  ```

### 3. NumNotEqual
- **Function Name**: NumNotEqual
- **Description**: Checks whether two integers are not equal; returns a boolean value.
- **Parameters**:
  - `num1`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: First integer to compare
    - Default: None
  - `num2`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Second integer to compare
    - Default: None
- **Returns**:
  - Type: BOOLEAN
  - Meaning: Returns `1` if the two integers differ; otherwise returns `0`
- **Example**:
  ```python
  a: int = 5
  b: int = 3
  result = NumNotEqual(a, b)   # result: 1
  ```

### 4. LessThan
- **Function Name**: LessThan
- **Description**: Checks whether the first integer is less than the second integer; returns a boolean value.
- **Parameters**:
  - `num1`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Left operand (value being compared)
    - Default: None
  - `num2`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Right operand (comparison value)
    - Default: None
- **Returns**:
  - Type: BOOLEAN
  - Meaning: Returns `1` if `num1 < num2`; otherwise returns `0`
- **Example**:
  ```python
  a: int = 3
  b: int = 5
  result = LessThan(a, b)    # result: 1
  ```

### 5. GreaterThan
- **Function Name**: GreaterThan
- **Description**: Checks whether the first integer is greater than the second integer; returns a boolean value.
- **Parameters**:
  - `num1`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Left operand (value being compared)
    - Default: None
  - `num2`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Right operand (comparison value)
    - Default: None
- **Returns**:
  - Type: BOOLEAN
  - Meaning: Returns `1` if `num1 > num2`; otherwise returns `0`
- **Example**:
  ```python
  a: int = 5
  b: int = 3
  result = GreaterThan(a, b)   # result: 1
  ```

### 6. LessOrEqual
- **Function Name**: LessOrEqual
- **Description**: Checks whether the first integer is less than or equal to the second integer; returns a boolean value.
- **Parameters**:
  - `num1`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Left operand (value being compared)
    - Default: None
  - `num2`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Right operand (comparison value)
    - Default: None
- **Returns**:
  - Type: BOOLEAN
  - Meaning: Returns `1` if `num1 <= num2`; otherwise returns `0`
- **Example**:
  ```python
  a: int = 3
  b: int = 5
  result = LessOrEqual(a, b)   # result: 1
  ```

### 7. GreaterOrEqual
- **Function Name**: GreaterOrEqual
- **Description**: Checks whether the first integer is greater than or equal to the second integer; returns a boolean value.
- **Parameters**:
  - `num1`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Left operand (value being compared)
    - Default: None
  - `num2`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Right operand (comparison value)
    - Default: None
- **Returns**:
  - Type: BOOLEAN
  - Meaning: Returns `1` if `num1 >= num2`; otherwise returns `0`
- **Example**:
  ```python
  a: int = 5
  b: int = 3
  result = GreaterOrEqual(a, b)   # result: 1
  ```

---

## VIII. Min/Max Functions

### 1. Min
- **Function Name**: Min
- **Description**: Returns the smaller of two integers.
- **Parameters**:
  - `a`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: First integer to compare
    - Default: None
  - `b`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Second integer to compare
    - Default: None
- **Returns**:
  - Type: INTEGER
  - Meaning: The smaller of the two input integers
- **Example**:
  ```python
  a: int = 3
  b: int = 5
  result= Min(3, 5)    # result: 3
  ```

### 2. Max
- **Function Name**: Max
- **Description**: Returns the larger of two integers.
- **Parameters**:
  - `a`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: First integer to compare
    - Default: None
  - `b`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Second integer to compare
    - Default: None
- **Returns**:
  - Type: INTEGER
  - Meaning: The larger of the two input integers
- **Example**:
  ```python
  a: int = 3
  b: int = 5
  result = Max(3, 5)    # result: 5
  ```

---

## IX. Range Check Functions

### 1. Within
- **Function Name**: Within
- **Description**: Checks whether the first integer is between the second integer (minimum) and third integer (maximum) (inclusive of boundaries).
- **Parameters**:
  - `min_val`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Minimum of the range (inclusive)
    - Default: None
  - `max_val`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Maximum of the range (exclusive)
    - Default: None
  - `num`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Integer to check if it's within the range
    - Default: None
- **Returns**:
  - Type: BOOLEAN
  - Meaning: Returns `1` if `num` is in `[min_val, max_val)` range; otherwise returns `0`
- **Example**:
  ```python
  a: int = 5
  b: int = 3
  c: int = 7
  result = Within(a, b, c)    # result: 1 (3 ≤ 5 < 7)
  ```

---

## X. Data Manipulation Functions

### 1. Cat
- **Function Name**: Cat
- **Description**: Concatenates two byte arrays into one byte array and returns it.
- **Parameters**:
  - `bytes1`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: Byte array to concatenate at the front
    - Default: None
  - `bytes2`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: Byte array to concatenate at the back
    - Default: None
- **Returns**:
  - Type: BYTES
  - Meaning: Concatenated byte array of `bytes1` and `bytes2`
- **Example**:
  ```python
  str1: string = "hello"
  str2: string = "world"
  result = Cat(str1, str2)  # result: "helloworld"
  ```

### 2. Split
- **Function Name**: Split
- **Description**: Splits a byte array at a specified length into two byte arrays and returns them.
- **Parameters**:
  - `bytes_data`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: Byte array to split
    - Default: None
  - `n`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Split length (must be ≥0)
    - Default: None
- **Returns**:
  - Type: BYTES, BYTES
  - Meaning: First is the first `n` bytes of `bytes_data`; second is the remaining bytes
- **Example**:
  ```python
  # Normal split
  str1: string = "helloworld"
  {part1, part2} = Split(str1, 5)
  # result: part1: "hello"  part2: "world"

  # Split length of 0
  str2: string = "test"
  {part1, part2} = Split(str2, 0)
  # result: part1: ""  part2: "test"

  # Split length equals array length
  str3: string = "test"
  {part1, part2} = Split("test", 4)
  # result: part1: "test"  part2: ""
  ```

### 3. Size
- **Function Name**: Size
- **Description**: Returns the length (in bytes) of the input byte array.
- **Parameters**:
  - `bytes_data`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: Byte array to measure
    - Default: None
- **Returns**:
  - Type: INTEGER
  - Meaning: Length of the byte array in bytes
- **Example**:
  ```python
  str: string = "hello"
  result: int = Size(str)  # output: 5 ("hello" contains 5 bytes)
  ```

---

## XI. Number Conversion Functions

### 1. NumToBin
- **Function Name**: NumToBin
- **Description**: Converts an integer to a byte array of the specified length and returns it.
- **Parameters**:
  - `num`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Integer to convert
    - Default: None
  - `length`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Target byte array length (must be ≥0)
    - Default: None
- **Returns**:
  - Type: BYTES
  - Meaning: Byte array of length `length` representing the binary encoding of `num`
- **Example**:
  ```python
  # Convert to 2-byte array
  a: int = 10
  result = NumToBin(a, 2)  # result: "0x0a00" (little-endian)

  # Pad with 0 when length is insufficient
  b: int = 5
  result = NumToBin(b, 4)  # result: "0x05000000"
  ```

### 2. BinToNum
- **Function Name**: BinToNum
- **Description**: Converts a byte array to an integer and returns it.
- **Parameters**:
  - `bytes_data`
    - Type: BYTES
    - Optional/Required: Required
    - Purpose: Byte array to convert to integer
    - Default: None
- **Returns**:
  - Type: INTEGER
  - Meaning: Integer parsed from the byte array
- **Example**:
  ```python
  bytes_data: hex = 0x1234
  result = BinToNum(bytes_data)  # result: 13330
  ```

---

## XII. Logical Operations

### 1. And
- **Function Name**: And
- **Description**: Performs a logical AND on two boolean values; returns the result.
- **Parameters**:
  - `a`
    - Type: BOOLEAN
    - Optional/Required: Required
    - Purpose: First boolean operand
    - Default: None
  - `b`
    - Type: BOOLEAN
    - Optional/Required: Required
    - Purpose: Second boolean operand
    - Default: None
- **Returns**:
  - Type: BOOLEAN
  - Meaning: Logical AND result of `a` and `b`
- **Example**:
  ```python
  a: bool = 1
  b: bool = 0
  result = And(a, b)  # result: 0
  ```

### 2. Or
- **Function Name**: Or
- **Description**: Performs a logical OR on two boolean values; returns the result.
- **Parameters**:
  - `a`
    - Type: BOOLEAN
    - Optional/Required: Required
    - Purpose: First boolean operand
    - Default: None
  - `b`
    - Type: BOOLEAN
    - Optional/Required: Required
    - Purpose: Second boolean operand
    - Default: None
- **Returns**:
  - Type: BOOLEAN
  - Meaning: Logical OR result of `a` and `b`
- **Example**:
  ```python
  a: bool = 1
  b: bool = 0
  result = Or(a, b)  # result: 1
  ```

---

## XIII. Other Built-in Functions

### 1. SetAlt
- **Function Name**: SetAlt
- **Description**: The main stack and alt stack can be viewed as a continuous linear storage space. This function only removes the specified target variable from the main stack and transfers it to the alt stack, keeping the relative order of remaining elements in the main stack unchanged.
- **Parameters**:
  - `variable`
    - Type: Any type
    - Optional/Required: Required
    - Purpose: Target variable in the main stack; this variable will be individually transferred to the alt stack
    - Default: None
- **Returns**:
  - Type: No return value
  - Meaning: Only adjusts the element composition of the main stack and alt stack (only transfers the specified variable); no specific return content
- **Example**:
  ```python
    # Initial state: main stack [x, y, a, c, b], alt stack [d, e], pointer at main stack top (right of b)
    SetAlt(a);
    # After execution: main stack [x, y, c, b], alt stack [a, c, b, d, e], pointer moves to position of a
  ```

### 2. SetMain
- **Function Name**: SetMain
- **Description**: Moves the operation pointer from the alt stack top to the position of the specified variable, bringing it and all elements to its left into the main stack.
- **Parameters**:
  - `variable`
    - Type: Any type
    - Optional/Required: Required
    - Purpose: Target variable in the alt stack, used to locate the new position of the operation pointer
    - Default: None
- **Returns**:
  - Type: No return value
  - Meaning: Only adjusts the boundary pointer between main and alt stacks; no specific return content
- **Example**:
  ```python
    # Initial state: main stack [x, y], alt stack [a, c, b, d, e], pointer at position of a in alt stack
    SetMain(b);
    # After execution: main stack [x, y, a, c, b], alt stack [d, e], pointer moves to position of b
  ```

### 3. Clone
- **Function Name**: Clone
- **Description**: Clones the specified object on the stack, generating an identical copy and pushing it to the stack top.
- **Parameters**:
  - No parameters (the function locates the target object via stack context)
- **Returns**:
  - Type: Same as the original object (default `BYTES`)
  - Meaning: Clone copy of the original object, pushed to stack top
- **Example**:
  ```python
    a = Push(10)
    result = a.Clone();  # clone variable a
  ```

### 4. Slice
- **Function Name**: Slice
- **Description**: Performs a slice operation on a byte array; supports three modes: "take from beginning", "take to end", and "take specified length".
- **Parameters**:
  - `start`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Slice starting position (-1 means from beginning; positive number means specific index)
    - Default: None
  - `end`
    - Type: INTEGER
    - Optional/Required: Required
    - Purpose: Slice ending position (-1 means to end; positive number means end index or length)
    - Default: None
- **Returns**:
  - Type: BYTES
  - Meaning: Sub-byte array obtained after slicing, pushed to stack top
- **Example**:
  ```python
    data: hex = 0x01020304
    # Take from beginning to position 2
    result1 = data.Slice(-1, 2);  # result: 0x0102

    # Take from position 1 to end
    result2 = data.Slice(1, -1);  # result: 0x020304

    # Take 2 bytes from position 1
    result3 = data.Slice(1, 2);   # result: 0x0203
  ```

### 5. Delete
- **Function Name**: Delete
- **Description**: Deletes one or more variables; supports recursive deletion of struct variables (automatically deletes all their fields).
- **Parameters**:
  - `variable1, variable2, ...`
    - Type: Any type
    - Optional/Required: Required (at least 1)
    - Purpose: Variables to delete; supports both regular variables and struct variables
    - Default: None
- **Returns**:
  - Type: No return value
  - Meaning: Only performs variable deletion; no specific return content
- **Example**:
  ```python
    # Delete a single regular variable
    Delete(x);  # delete variable x from main or alt stack

    # Delete a struct variable (recursive deletion of fields)
    # Struct person exists with person.name (string) and person.age (integer)
    Delete(person);  # simultaneously deletes person, person.name, person.age

    # Batch delete multiple variables
    Delete(a, b, c);  # batch delete variables a, b, c
  ```

### 6. Push
- **Function Name**: Push
- **Description**: Pushes the specified value (literal or variable) from the fixed area to the main stack; only supports data from the fixed area.
- **Parameters**:
  - `value`
    - Type: Any type (literal or fixed-area variable)
    - Optional/Required: Required
    - Purpose: Data to push from the fixed area to the main stack
    - Default: None
- **Returns**:
  - Type: BYTES
  - Meaning: Data pushed to the main stack (same type as input, default treated as `BYTES`)
- **Example**:
  ```python
    # Push a literal
    result = Push(10);  # push 10 to main stack

    # Push a fixed-area variable
    # Prerequisite: variable msg exists in fixed area
    result = Push(msg);  # push the value of msg to main stack
  ```

### 7. Keep
- **Function Name**: Keep
- **Description**: Zero-cost abstract function for marking and retaining a specified number of variables.
- **Parameters**:
  - `variable1, variable2, ...`
    - Type: Any type (default treated as `BYTES`)
    - Optional/Required: Required (at least 1)
    - Purpose: Variables to mark as retained
    - Default: None
- **Returns**:
  - Type: Same type as input parameters (default `BYTES`)
  - Meaning: Returns values of exactly the same count and type as input; only marks retention status
- **Example**:
  ```python
    # Retain a single variable
    a = Push(10)
    Keep(a);  # mark variable a as retained

    # Retain multiple variables
    x = Push(123)
    y = Push("hello")
    Keep(x, y);  # mark variables x, y as retained
  ```

---

## Next Steps

- [Built-in Objects Reference](./builtin-objects.md) — Full reference for `self` / `BVM` and other built-in objects

---

[🇨🇳 中文版](../../zh/references/builtin-functions.md)
