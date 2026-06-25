# 内置函数参考

---

本文档列出 UTXO_Compiler 合约语言的所有内置函数。包括函数功能、参数说明、返回值、异常场景及使用示例。涵盖加密、签名验证、算术运算、比较操作和栈操作（主栈与副栈）、数据克隆、切片、删除、推送等多个类别，旨在为开发人员提供清晰、准确的函数使用指南。

---

## 一、加密函数

### 1. Rmd160
- **函数名（Function Name）**：Rmd160
- **描述（Short Description）**：计算输入字节数组的RIPEMD160哈希值。
- **参数（Parameters）**：
  - `input_data`
    - 类型（Type）：BYTES
    - 是否可选（Optional/Required）：Required
    - 用途：待计算哈希的原始数据
    - 默认值：无
- **返回值（Returns）**：
  - 类型：BYTES（长度固定为20字节）
  - 含义：输入数据的RIPEMD160哈希结果
- **使用示例（Example）**：  
  ```python
  input_bytes: hex = 0x1234567890
  result = Rmd160(input_bytes)
  ```  

### 2. Sha1
- **函数名（Function Name）**：Sha1
- **描述（Short Description）**：计算输入字节数组的SHA1哈希值。
- **参数（Parameters）**：
  - `input_data`
    - 类型（Type）：BYTES
    - 是否可选（Optional/Required）：Required
    - 用途：待计算哈希的原始数据
    - 默认值：无
- **返回值（Returns）**：
  - 类型：BYTES（长度固定为20字节）
  - 含义：输入数据的SHA1哈希结果
- **使用示例（Example）**：
  ```python
  input_bytes: hex = 0x1234567890
  result = Sha1(input_bytes)
  ```  

### 3. Sha256
- **函数名（Function Name）**：Sha256  
- **描述（Short Description）**：计算输入字节数组的SHA256哈希值。    
- **参数（Parameters）**：
  - `input_data`
    - 类型（Type）：BYTES
    - 是否可选（Optional/Required）：Required
    - 用途：待计算哈希的原始数据
    - 默认值：无
- **返回值（Returns）**：
  - 类型：BYTES（长度固定为32字节）
  - 含义：输入数据的SHA256哈希结果
- **使用示例（Example）**：
  ```python
  input_bytes: hex = 0x1234567890
  result = Sha256(input_bytes)
  ```  

### 4. Hash160
- **函数名（Function Name）**：Hash160  
- **描述（Short Description）**：对输入字节数组执行SHA256后接RIPEMD160的组合哈希计算。   
- **参数（Parameters）**：  
  - `input_data`  
    - 类型（Type）：BYTES  
    - 是否可选（Optional/Required）：Required  
    - 用途：待计算组合哈希的原始数据 
    - 默认值：无 
- **返回值（Returns）**：  
  - 类型：BYTES（长度固定为20字节）  
  - 含义：输入数据经SHA256+RIPEMD160计算后的哈希结果     
- **使用示例（Example）**：  
  ```python
  input_bytes: hex = 0x1234567890
  result = Hash160(input_bytes)
  ```  

### 5. Hash256
- **函数名（Function Name）**：Hash256  
- **描述（Short Description）**：对输入字节数组执行两次SHA256哈希计算。    
- **参数（Parameters）**：  
  - `input_data`  
    - 类型（Type）：BYTES  
    - 是否可选（Optional/Required）：Required  
    - 用途：待计算双重SHA256的原始数据 
    - 默认值：无 
- **返回值（Returns）**：  
  - 类型：BYTES（长度固定为32字节）  
  - 含义：输入数据经两次SHA256计算后的哈希结果  
- **使用示例（Example）**：  
  ```python
  input_bytes: hex = 0x1234567890
  result = Hash256(input_bytes)
  ```  

### 6. PartialHash
- **函数名（Function Name）**：PartialHash  
- **描述（Short Description）**：对三个输入字节数组执行部分哈希组合计算。  
- **参数（Parameters）**：  
  - `part1`  
    - 类型（Type）：BYTES  
    - 是否可选（Optional/Required）：Required  
    - 用途：哈希计算的第一部分数据 
    - 默认值：无 
  - `part2`  
    - 类型（Type）：BYTES  
    - 是否可选（Optional/Required）：Required  
    - 用途：哈希计算的第二部分数据 
    - 默认值：无 
  - `part3`  
    - 类型（Type）：BYTES  
    - 是否可选（Optional/Required）：Required  
    - 用途：哈希计算的第三部分数据  
    - 默认值：无
- **返回值（Returns）**：  
  - 类型：BYTES  
  - 含义：三个输入部分组合后的部分哈希结果  
- **使用示例（Example）**：  
  ```python
  p1: hex = 0x1234
  p2: hex = 0x5678
  p3: hex = 0x9012
  result = PartialHash(p1, p2, p3)
  ```  

---

## 二、签名验证函数

### 1. CheckSig
- **函数名（Function Name）**：CheckSig  
- **描述（Short Description）**：验证签名与公钥是否匹配，返回布尔值结果。
- **参数（Parameters）**：  
  - `signature`  
    - 类型（Type）：BYTES  
    - 是否可选（Optional/Required）：Required  
    - 用途：待验证的签名数据  
    - 默认值：无
  - `public_key`  
    - 类型（Type）：BYTES  
    - 是否可选（Optional/Required）：Required  
    - 用途：用于验证签名的公钥 
    - 默认值：无 
- **返回值（Returns）**：  
  - 类型：BOOLEAN  
  - 含义：`1`表示签名有效，`0`表示签名无效  
- **使用示例（Example）**：  
  ```python
  sig: hex = 0x1234       # 假设为签名字节数组
  pub_key: hex = 0x5678   # 对应公钥
  result = CheckSig(sig, pub_key)
  ```  

### 2. CheckSigVerify
- **函数名（Function Name）**：CheckSigVerify  
- **描述（Short Description）**：验证签名与公钥是否匹配，验证失败则终止执行。    
- **参数（Parameters）**：  
  - `signature`  
    - 类型（Type）：BYTES  
    - 是否可选（Optional/Required）：Required  
    - 用途：待验证的签名数据 
    - 默认值：无
  - `public_key`  
    - 类型（Type）：BYTES  
    - 是否可选（Optional/Required）：Required  
    - 用途：用于验证签名的公钥  
    - 默认值：无
- **返回值（Returns）**：无  
- **使用示例（Example）**：  
  ```python
  # 验证有效签名（正常执行）
  valid_sig: hex = 0x1234
  pub_key: hex = 0x5678
  CheckSigVerify(valid_sig, pub_key)  # 无输出，继续执行

  # 验证无效签名（抛出异常并终止）
  invalid_sig: hex = 0x9012
  CheckSigVerify(invalid_sig, pub_key)  # 终止执行
  ```  

### 3. MultiSig
- **函数名（Function Name）**：MultiSig  
- **描述（Short Description）**：验证多签名集合与公钥集合是否匹配（通常需满足"n个签名中至少m个有效"的逻辑），返回布尔值结果。    
- **参数（Parameters）**：  
  - `signatures`  
    - 类型（Type）：BYTES（包含多个签名的序列化数据）  
    - 是否可选（Optional/Required）：Required  
    - 用途：待验证的多签名集合  
    - 默认值：无
  - `public_keys`  
    - 类型（Type）：BYTES（包含多个公钥的序列化数据）  
    - 是否可选（Optional/Required）：Required  
    - 用途：用于验证的公钥集合  
    - 默认值：无
- **返回值（Returns）**：  
  - 类型：BOOLEAN  
  - 含义：`1`表示多签名验证通过，`0`表示不通过  
- **使用示例（Example）**：  
  ```python
  sigs: hex = 0x1234567890      # 序列化的有效签名集合
  pub_keys: hex = 0x0987654321  # 序列化的公钥集合
  result = MultiSig(sigs, pub_keys)
  ```  

### 4. MultiSigVerify
- **函数名（Function Name）**：MultiSigVerify  
- **描述（Short Description）**：验证多签名集合与公钥集合是否匹配，验证失败则终止执行。  
- **参数（Parameters）**：  
  - `signatures`  
    - 类型（Type）：BYTES（包含多个签名的序列化数据）  
    - 是否可选（Optional/Required）：Required  
    - 用途：待验证的多签名集合  
    - 默认值：无
  - `public_keys`  
    - 类型（Type）：BYTES（包含多个公钥的序列化数据）  
    - 是否可选（Optional/Required）：Required  
    - 用途：用于验证的公钥集合  
    - 默认值：无
- **返回值（Returns）**：无     
- **使用示例（Example）**：  
  ```python
  # 验证有效多签名（正常执行）
  valid_sigs: hex = 0x1234567890
  pub_keys: hex = 0x0987654321
  MultiSigVerify(valid_sigs, pub_keys)  # 无输出，继续执行

  # 验证无效多签名（抛出异常并终止）
  invalid_sigs: hex = 0x6789012345
  MultiSigVerify(invalid_sigs, pub_keys)  # 终止执行
  ```  

---

## 三、单操作数算术运算函数

### 1. Inc
- **函数名（Function Name）**：Inc  
- **描述（Short Description）**：对输入整数执行加1操作并返回结果。  
- **参数（Parameters）**：  
  - `x`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：待加1的整数  
    - 默认值：无
- **返回值（Returns）**：  
  - 类型：INTEGER  
  - 含义：输入整数加1后的结果   
- **使用示例（Example）**：  
  ```python
  a: int = 5
  result = Inc(a)   # 结果：6
  ```  

### 2. Dec
- **函数名（Function Name）**：Dec  
- **描述（Short Description）**：对输入整数执行减1操作并返回结果。  
- **参数（Parameters）**：  
  - `x`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：待减1的整数  
    - 默认值：无
- **返回值（Returns）**：  
  - 类型：INTEGER  
  - 含义：输入整数减1后的结果   
- **使用示例（Example）**：  
  ```python
  a: int = 5
  result = Dec(a)   # 结果：4
  ```  

### 3. Neg
- **函数名（Function Name）**：Neg  
- **描述（Short Description）**：对输入整数取相反数并返回结果。  
- **参数（Parameters）**：  
  - `x`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：待取相反数的整数
    - 默认值：无  
- **返回值（Returns）**：  
  - 类型：INTEGER  
  - 含义：输入整数的相反数    
- **使用示例（Example）**：  
  ```python
  a: int = 5
  result = Neg(a)   # 结果：-5
  ```  

### 4. Abs
- **函数名（Function Name）**：Abs  
- **描述（Short Description）**：对输入整数取绝对值并返回结果。   
- **参数（Parameters）**：  
  - `x`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：待取绝对值的整数  
    - 默认值：无
- **返回值（Returns）**：  
  - 类型：INTEGER  
  - 含义：输入整数的绝对值   
- **使用示例（Example）**：  
  ```python
  a: int = -5
  result = Abs(a)   # 结果：5
  ```  

### 5. Not
- **函数名（Function Name）**：Not  
- **描述（Short Description）**：对输入整数执行逻辑非操作，返回布尔值。   
- **参数（Parameters）**：  
  - `x`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：待执行逻辑非的整数  
    - 默认值：无
- **返回值（Returns）**：  
  - 类型：BOOLEAN  
  - 含义：输入为0时返回`1`，否则返回`0`
- **使用示例（Example）**：  
  ```python
  a: int = 5
  result = Not(a)   # 结果：0
  ```  

### 6. ZeroNotEqual
- **函数名（Function Name）**：ZeroNotEqual  
- **描述（Short Description）**：判断输入整数是否不等于0，返回布尔值。  
- **参数（Parameters）**：  
  - `x`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：待判断的整数  
    - 默认值：无
- **返回值（Returns）**：  
  - 类型：BOOLEAN  
  - 含义：输入非0时返回`1`，输入为0时返回`0` 
- **使用示例（Example）**：  
  ```python
  a: int = 0
  result = ZeroNotEqual(a)   # 结果：0
  ```  

---

## 四、双操作数算术运算函数

### 1. Add
- **函数名（Function Name）**：Add  
- **描述（Short Description）**：计算两个整数的和并返回结果。  
- **参数（Parameters）**：  
  - `a`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：第一个加数  
    - 默认值：无
  - `b`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：第二个加数  
    - 默认值：无
- **返回值（Returns）**：  
  - 类型：INTEGER  
  - 含义：两个输入整数的和   
- **使用示例（Example）**：  
  ```python
  a: int = 2
  b: int = 3
  result = Add(a, b)   # 结果：5
  ```  

### 2. Sub
- **函数名（Function Name）**：Sub  
- **描述（Short Description）**：计算第一个整数减去第二个整数的差并返回结果。    
- **参数（Parameters）**：  
  - `a`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：被减数  
    - 默认值：无
  - `b`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：减数  
    - 默认值：无
- **返回值（Returns）**：  
  - 类型：INTEGER  
  - 含义：`a - b`的计算结果  
- **使用示例（Example）**：  
  ```python
  a: int = 5
  b: int = 3
  result = Sub(a, b)   # 结果：2
  ```  

### 3. Mul
- **函数名（Function Name）**：Mul  
- **描述（Short Description）**：计算两个整数的乘积并返回结果。   
- **参数（Parameters）**：  
  - `a`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：第一个乘数  
    - 默认值：无
  - `b`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：第二个乘数  
    - 默认值：无
- **返回值（Returns）**：  
  - 类型：INTEGER  
  - 含义：两个输入整数的乘积    
- **使用示例（Example）**：  
  ```python
  a: int = 2
  b: int = 3
  result = Mul(a, b)   # 结果：6
  ```  

### 4. Div
- **函数名（Function Name）**：Div  
- **描述（Short Description）**：计算第一个整数除以第二个整数的商并返回结果。   
- **参数（Parameters）**：  
  - `a`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：被除数  
    - 默认值：无
  - `b`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：除数（不能为0）  
    - 默认值：无
- **返回值（Returns）**：  
  - 类型：INTEGER  
  - 含义：`a`除以`b`的商（整数部分）    
- **使用示例（Example）**：  
  ```python
  a: int = 10
  b: int = 2
  result = Div(a, b)   # 结果：5
  ```  

### 5. Mod
- **函数名（Function Name）**：Mod  
- **描述（Short Description）**：计算第一个整数除以第二个整数的余数并返回结果。   
- **参数（Parameters）**：  
  - `a`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：被除数  
    - 默认值：无
  - `b`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：除数（不能为0） 
    - 默认值：无 
- **返回值（Returns）**：  
  - 类型：INTEGER  
  - 含义：`a`除以`b`的余数  
- **使用示例（Example）**：  
  ```python
  a: int = 10
  b: int = 3
  result = Mod(a, b)   # 结果：1
  ```  

---

## 五、位移运算函数

### 1. Lshift
- **函数名（Function Name）**：Lshift  
- **描述（Short Description）**：将第一个整数按第二个整数指定的位数左移并返回结果。  
- **参数（Parameters）**：  
  - `a`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：待左移的整数  
    - 默认值：无
  - `b`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：移位位数（必须≥0）
    - 默认值：无  
- **返回值（Returns）**：  
  - 类型：INTEGER  
  - 含义：`a`左移`b`位后的结果  
- **使用示例（Example）**：  
  ```python
  a: int = 4
  b: int = 2
  result = Lshift(a, b)   # 结果：16
  ```  

### 2. Rshift
- **函数名（Function Name）**：Rshift  
- **描述（Short Description）**：将第一个整数按第二个整数指定的位数右移并返回结果。   
- **参数（Parameters）**：  
  - `a`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：待右移的整数 
    - 默认值：无 
  - `b`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：移位位数（必须≥0） 
    - 默认值：无 
- **返回值（Returns）**：  
  - 类型：INTEGER  
  - 含义：`a`右移`b`位后的结果  
- **使用示例（Example）**：  
  ```python
  a: int = 8
  b: int = 3
  result = Rshift(a, b)   # 结果：1
  ```  

---

## 六、非数值比较函数

### 1. Equal
- **函数名（Function Name）**：Equal  
- **描述（Short Description）**：判断两个字节数组是否相等，返回布尔值。   
- **参数（Parameters）**：  
  - `bytes1`  
    - 类型（Type）：BYTES  
    - 是否可选（Optional/Required）：Required  
    - 用途：第一个待比较的字节数组  
    - 默认值：无
  - `bytes2`  
    - 类型（Type）：BYTES  
    - 是否可选（Optional/Required）：Required  
    - 用途：第二个待比较的字节数组  
    - 默认值：无
- **返回值（Returns）**：  
  - 类型：BOOLEAN  
  - 含义：两个字节数组完全相同时返回`1`，否则返回`0`
- **使用示例（Example）**：  
  ```python
  a: string = "hello"
  b: string = "hello"
  result = Equal(a, b)   # 结果：1
  ```  

### 2. EqualVerify
- **函数名（Function Name）**：EqualVerify  
- **描述（Short Description）**：判断两个字节数组是否相等，不相等则终止执行。    
- **参数（Parameters）**：  
  - `bytes1`  
    - 类型（Type）：BYTES  
    - 是否可选（Optional/Required）：Required  
    - 用途：第一个待比较的字节数组  
    - 默认值：无
  - `bytes2`  
    - 类型（Type）：BYTES  
    - 是否可选（Optional/Required）：Required  
    - 用途：第二个待比较的字节数组 
    - 默认值：无 
- **返回值（Returns）**：无   
- **使用示例（Example）**：  
  ```python
  # 相等的情况（正常执行）
  a: string = "hello"
  b: string = "hello"
  EqualVerify(a, b)  # 无输出，继续执行

  # 不相等的情况（抛出异常并终止）
  a: string = "hello"
  b: string = "world"
  EqualVerify(a, b)  # 终止执行
  ```  

---

## 七、数值比较函数

### 1. NumEqual
- **函数名（Function Name）**：NumEqual  
- **描述（Short Description）**：判断两个整数是否相等，返回布尔值。    
- **参数（Parameters）**：  
  - `num1`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：第一个待比较的整数  
    - 默认值：无
  - `num2`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：第二个待比较的整数  
    - 默认值：无
- **返回值（Returns）**：  
  - 类型：BOOLEAN  
  - 含义：两个整数数值相同时返回`1`，否则返回`0`
- **使用示例（Example）**：  
  ```python
  a: int = 5
  b: int = 5
  result = NumEqual(a, b)   # 结果：1
  ```  

### 2. NumEqualVerify
- **函数名（Function Name）**：NumEqualVerify  
- **描述（Short Description）**：判断两个整数是否相等，不相等则终止执行。    
- **参数（Parameters）**：  
  - `num1`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：第一个待比较的整数  
    - 默认值：无
  - `num2`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：第二个待比较的整数  
    - 默认值：无
- **返回值（Returns）**：无      
- **使用示例（Example）**：  
  ```python
  # 相等的情况（正常执行）
  a: int = 5
  b: int = 5
  NumEqualVerify(a, b)  # 无输出，继续执行

  # 不相等的情况（抛出异常并终止）
  a: int = 5
  b: int = 3
  NumEqualVerify(a, b)  # 终止执行
  ```  

### 3. NumNotEqual
- **函数名（Function Name）**：NumNotEqual  
- **描述（Short Description）**：判断两个整数是否不相等，返回布尔值。  
- **参数（Parameters）**：  
  - `num1`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：第一个待比较的整数  
    - 默认值：无
  - `num2`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：第二个待比较的整数
    - 默认值：无  
- **返回值（Returns）**：  
  - 类型：BOOLEAN  
  - 含义：两个整数数值不同时返回`1`，否则返回`0`    
- **使用示例（Example）**：  
  ```python
  a: int = 5
  b: int = 3
  result = NumNotEqual(a, b)   # 结果：1
  ```  

### 4. LessThan
- **函数名（Function Name）**：LessThan  
- **描述（Short Description）**：判断第一个整数是否小于第二个整数，返回布尔值。   
- **参数（Parameters）**：  
  - `num1`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：比较中的左操作数（被比较数） 
    - 默认值：无 
  - `num2`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：比较中的右操作数（比较数） 
    - 默认值：无 
- **返回值（Returns）**：  
  - 类型：BOOLEAN  
  - 含义：`num1 < num2`成立时返回`1`，否则返回`0`    
- **使用示例（Example）**：  
  ```python
  a: int = 3
  b: int = 5
  result = LessThan(a, b)    # 结果：1
  ```  

### 5. GreaterThan
- **函数名（Function Name）**：GreaterThan  
- **描述（Short Description）**：判断第一个整数是否大于第二个整数，返回布尔值。    
- **参数（Parameters）**：  
  - `num1`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：比较中的左操作数（被比较数） 
    - 默认值：无 
  - `num2`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：比较中的右操作数（比较数）
    - 默认值：无  
- **返回值（Returns）**：  
  - 类型：BOOLEAN  
  - 含义：`num1 > num2`成立时返回`1`，否则返回`0`    
- **使用示例（Example）**：  
  ```python
  a: int = 5
  b: int = 3
  result = GreaterThan(a, b)   # 结果：1
  ```  

### 6. LessOrEqual
- **函数名（Function Name）**：LessOrEqual  
- **描述（Short Description）**：判断第一个整数是否小于等于第二个整数，返回布尔值。  
- **参数（Parameters）**：  
  - `num1`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：比较中的左操作数（被比较数）
    - 默认值：无  
  - `num2`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：比较中的右操作数（比较数）
    - 默认值：无  
- **返回值（Returns）**：  
  - 类型：BOOLEAN  
  - 含义：`num1 <= num2`成立时返回`1`，否则返回`0`     
- **使用示例（Example）**：  
  ```python
  a: int = 3
  b: int = 5
  result = LessOrEqual(a, b)   # 结果：1
  ```  

### 7. GreaterOrEqual
- **函数名（Function Name）**：GreaterOrEqual  
- **描述（Short Description）**：判断第一个整数是否大于等于第二个整数，返回布尔值。    
- **参数（Parameters）**：  
  - `num1`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：比较中的左操作数（被比较数）
    - 默认值：无  
  - `num2`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：比较中的右操作数（比较数）
    - 默认值：无  
- **返回值（Returns）**：  
  - 类型：BOOLEAN  
  - 含义：`num1 >= num2`成立时返回`1`，否则返回`0`     
- **使用示例（Example）**：  
  ```python
  a: int = 5
  b: int = 3
  result = GreaterOrEqual(a, b)   # 结果：1
  ```  

---

## 八、最值函数

### 1. Min
- **函数名（Function Name）**：Min  
- **描述（Short Description）**：返回两个整数中的较小值。    
- **参数（Parameters）**：  
  - `a`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：第一个待比较的整数
    - 默认值：无  
  - `b`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：第二个待比较的整数 
    - 默认值：无 
- **返回值（Returns）**：  
  - 类型：INTEGER  
  - 含义：两个输入整数中的较小值      
- **使用示例（Example）**：  
  ```python
  a: int = 3
  b: int = 5
  result= Min(3, 5)    # 结果：3
  ```  

### 2. Max
- **函数名（Function Name）**：Max  
- **描述（Short Description）**：返回两个整数中的较大值。  
- **参数（Parameters）**：  
  - `a`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：第一个待比较的整数 
    - 默认值：无 
  - `b`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：第二个待比较的整数 
    - 默认值：无 
- **返回值（Returns）**：  
  - 类型：INTEGER  
  - 含义：两个输入整数中的较大值      
- **使用示例（Example）**：  
  ```python
  a: int = 3
  b: int = 5
  result = Max(3, 5)    # 结果：5
  ```  

---

## 九、范围检查函数

### 1. Within
- **函数名（Function Name）**：Within  
- **描述（Short Description）**：判断第一个整数是否在第二个整数（最小值）和第三个整数（最大值）之间（含边界）。   
- **参数（Parameters）**：  
  - `min_val`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：范围的最小值（包含）
    - 默认值：无  
  - `max_val`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：范围的最大值（包含） 
    - 默认值：无 
  - `num`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：待检查是否在范围内的整数 
    - 默认值：无 
- **返回值（Returns）**：  
  - 类型：BOOLEAN  
  - 含义：`num`在`[min_val, max_val）`范围内时返回`1`，否则返回`0`  
- **使用示例（Example）**：  
  ```python
  a: int = 5
  b: int = 3
  c: int = 7
  result = Within(a, b, c)    # 结果：1（3 ≤ 5 < 7）
  ```  

---

## 十、数据操作函数

### 1. Cat
- **函数名（Function Name）**：Cat  
- **描述（Short Description）**：将两个字节数组拼接为一个字节数组并返回。
- **参数（Parameters）**：  
  - `bytes1`  
    - 类型（Type）：BYTES  
    - 是否可选（Optional/Required）：Required  
    - 用途：拼接在前面的字节数组 
    - 默认值：无 
  - `bytes2`  
    - 类型（Type）：BYTES  
    - 是否可选（Optional/Required）：Required  
    - 用途：拼接在后面的字节数组 
    - 默认值：无 
- **返回值（Returns）**：  
  - 类型：BYTES  
  - 含义：`bytes1`与`bytes2`拼接后的字节数组   
- **使用示例（Example）**：  
  ```python
  str1: string = "hello"
  str2: string = "world"
  result = Cat(str1, str2)  # 结果："helloworld"
  ```  

### 2. Split
- **函数名（Function Name）**：Split  
- **描述（Short Description）**：将字节数组按指定长度分割为两个字节数组并返回。  
- **参数（Parameters）**：  
  - `bytes_data`  
    - 类型（Type）：BYTES  
    - 是否可选（Optional/Required）：Required  
    - 用途：待分割的字节数组 
    - 默认值：无 
  - `n`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：分割长度（必须≥0）
    - 默认值：无  
- **返回值（Returns）**：  
  - 类型：BYTES, BYTES  
  - 含义：第一个为`bytes_data`的前`n`个字节，第二个为剩余字节  
- **使用示例（Example）**：  
  ```python
  # 正常分割
  str1: string = "helloworld"
  {part1, part2} = Split(str1, 5)
  # 结果：part1: "hello"  part2: world"

  # 分割长度为0
  str2: string = "test"
  {part1, part2} = Split(str2, 0)
  # 结果：part1: ""  part2: "test"

  # 分割长度为原数组长度
  str3: string = "test"
  {part1, part2} = Split("test", 4)
  # 结果：part1: "test"  part2: ""
  ```  

### 3. Size
- **函数名（Function Name）**：Size  
- **描述（Short Description）**：返回输入字节数组的长度（字节数）。  
- **参数（Parameters）**：  
  - `bytes_data`  
    - 类型（Type）：BYTES  
    - 是否可选（Optional/Required）：Required  
    - 用途：待计算长度的字节数组  
    - 默认值：无
- **返回值（Returns）**：  
  - 类型：INTEGER  
  - 含义：字节数组的长度（字节数）     
- **使用示例（Example）**：  
  ```python
  str: string = "hello"
  result: int = Size(str)  # 输出：5（"hello"包含5个字节）
  ``` 

---

## 十一、数字转换函数

### 1. NumToBin
- **函数名（Function Name）**：NumToBin  
- **描述（Short Description）**：将整数转换为指定长度的字节数组并返回。   
- **参数（Parameters）**：  
  - `num`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：待转换的整数 
    - 默认值：无 
  - `length`  
    - 类型（Type）：INTEGER  
    - 是否可选（Optional/Required）：Required  
    - 用途：目标字节数组的长度（必须≥0）
    - 默认值：无  
- **返回值（Returns）**：  
  - 类型：BYTES  
  - 含义：长度为`length`的、`num`的二进制表示字节数组   
- **使用示例（Example）**：  
  ```python
  # 转换为2字节数组
  a: int = 10
  result = NumToBin(a, 2)  # 结果："0x0a00" （小端序）

  # 长度不足时补0
  b: int = 5
  result = NumToBin(b, 4)  # 结果："0x05000000"
  ```  

### 2. BinToNum
- **函数名（Function Name）**：BinToNum  
- **描述（Short Description）**：将字节数组转换为整数并返回。   
- **参数（Parameters）**：  
  - `bytes_data`  
    - 类型（Type）：BYTES  
    - 是否可选（Optional/Required）：Required  
    - 用途：待转换为整数的字节数组 
    - 默认值：无 
- **返回值（Returns）**：  
  - 类型：INTEGER  
  - 含义：字节数组解析后的整数    
- **使用示例（Example）**：  
  ```python
  bytes_data: hex = 0x1234
  result = BinToNum(bytes_data)  # 结果：13330
  ```  

---

## 十二、逻辑运算

### 1. And
- **函数名（Function Name）**：And  
- **描述（Short Description）**：对两个布尔值执行逻辑与操作，返回结果。
- **参数（Parameters）**：  
  - `a`  
    - 类型（Type）：BOOLEAN  
    - 是否可选（Optional/Required）：Required  
    - 用途：第一个布尔操作数  
    - 默认值：无
  - `b`  
    - 类型（Type）：BOOLEAN  
    - 是否可选（Optional/Required）：Required  
    - 用途：第二个布尔操作数 
    - 默认值：无 
- **返回值（Returns）**：  
  - 类型：BOOLEAN  
  - 含义：`a`与`b`的逻辑与结果    
- **使用示例（Example）**：  
  ```python
  a: bool = 1
  b: bool = 0
  result = And(a, b)  # 结果：0
  ```  

### 2. Or
- **函数名（Function Name）**：Or  
- **描述（Short Description）**：对两个布尔值执行逻辑或操作，返回结果。  
- **参数（Parameters）**：  
  - `a`  
    - 类型（Type）：BOOLEAN  
    - 是否可选（Optional/Required）：Required  
    - 用途：第一个布尔操作数
    - 默认值：无  
  - `b`  
    - 类型（Type）：BOOLEAN  
    - 是否可选（Optional/Required）：Required  
    - 用途：第二个布尔操作数  
    - 默认值：无
- **返回值（Returns）**：  
  - 类型：BOOLEAN  
  - 含义：`a`与`b`的逻辑或结果    
- **使用示例（Example）**：  
  ```python
  a: bool = 1
  b: bool = 0
  result = Or(a, b)  # 结果：1
  ```

---

## 十三、其它内置函数

### 1. SetAlt
- **函数名（Function Name）**：SetAlt
- **描述（Short Description）**：主栈与副栈可视为一个连续的线性存储空间。该函数仅将主栈中指定的目标变量从主栈移除，并划归至副栈中，主栈内其余元素的相对顺序保持不变。
- **参数（Parameters）**：
  - `variable`
    - 类型（Type）：任意类型
    - 是否可选（Optional/Required）：Required
    - 用途：主栈中的目标变量，该变量会被单独迁移至副栈
    - 默认值：无
- **返回值（Returns）**：
  - 类型：无返回值
  - 含义：仅调整主栈和副栈的元素构成（仅迁移指定变量），无具体返回内容
- **使用示例（Example）**：
  ```python
    # 初始状态：主栈[ x, y, a, c, b ]，副栈[ d, e ]，指针在主栈栈顶（b右侧）
    SetAlt(a);  
    # 执行后：主栈[ x, y, c, b ]，副栈[ a, c, b, d, e ]，指针移动到a的位置
  ```

### 2. SetMain
- **函数名（Function Name）**：SetMain
- **描述（Short Description）**：将操作指针从副栈栈顶移动到指定变量的位置，使其及左侧元素划归主栈。
- **参数（Parameters）**：
  - `variable`
    - 类型（Type）：任意类型
    - 是否可选（Optional/Required）：Required
    - 用途：副栈中的目标变量，用于定位操作指针的新位置
    - 默认值：无
- **返回值（Returns）**：
  - 类型：无返回值
  - 含义：仅调整主、副栈的边界指针，无具体返回内容
- **使用示例（Example）**：
  ```python
    # 初始状态：主栈[ x, y ]，副栈[ a, c, b, d, e ]，指针在副栈中a的位置
    SetMain(b);  
    # 执行后：主栈[ x, y, a, c, b ]，副栈[ d, e ]，指针移动到b的位置
  ```

### 3. Clone
- **函数名（Function Name）**：Clone
- **描述（Short Description）**：克隆栈中指定对象，生成与原对象完全一致的副本并压入栈顶。
- **参数（Parameters）**：
  - 无参数（函数通过栈上下文定位目标对象）
- **返回值（Returns）**：
  - 类型：与原对象一致（默认`BYTES`）
  - 含义：原对象的克隆副本，压入栈顶
- **使用示例（Example）**：
  ```python
    a = Push(10)
    result = a.Clone();  # 克隆变量a
  ```

### 4. Slice
- **函数名（Function Name）**：Slice
- **描述（Short Description）**：对字节数组执行切片操作，支持“从开头截取”“截取到末尾”“指定长度截取”三种模式。
- **参数（Parameters）**：
  - `start`
    - 类型（Type）：INTEGER
    - 是否可选（Optional/Required）：Required
    - 用途：切片起始位置（-1表示从开头，正数表示具体索引）
    - 默认值：无
  - `end`
    - 类型（Type）：INTEGER
    - 是否可选（Optional/Required）：Required
    - 用途：切片结束位置（-1表示到末尾，正数表示结束索引或长度）
    - 默认值：无
- **返回值（Returns）**：
  - 类型：BYTES
  - 含义：切片后得到的子字节数组，压入栈顶
- **使用示例（Example）**：
  ```python
    data: hex = 0x01020304
    #从开头截取到位置2
    result1 = data.Slice(-1, 2);  # 结果：0x0102

    #从位置1截取到末尾
    result2 = data.Slice(1, -1);  # 结果：0x020304

    #从位置1截取2个字节
    result3 = data.Slice(1, 2);   # 结果：0x0203
  ```

### 5. Delete
- **函数名（Function Name）**：Delete
- **描述（Short Description）**：删除一个或多个变量，支持结构体变量的递归删除（自动删除其所有字段）。
- **参数（Parameters）**：
  - `variable1, variable2, ...`
    - 类型（Type）：任意类型
    - 是否可选（Optional/Required）：Required（至少1个）
    - 用途：需要删除的变量，支持普通变量和结构体变量
    - 默认值：无
- **返回值（Returns）**：
  - 类型：无返回值
  - 含义：仅执行变量删除操作，无具体返回内容
- **使用示例（Example）**：
  ```python
    # 删除单个普通变量
    Delete(x);  # 删除主栈或副栈中的变量x

    # 删除结构体变量（递归删除字段）
    # 存在结构体person，包含person.name（字符串）、person.age（整数）
    Delete(person);  # 同时删除person、person.name、person.age

    # 批量删除多变量
    Delete(a, b, c);  # 批量删除变量a、b、c
  ```

### 6. Push
- **函数名（Function Name）**：Push
- **描述（Short Description）**：将指定值（字面量或变量）从固定区（fixed area）推送至主栈，仅支持固定区数据。
- **参数（Parameters）**：
  - `value`
    - 类型（Type）：任意类型（字面量或固定区变量）
    - 是否可选（Optional/Required）：Required
    - 用途：需要从固定区推送至主栈的数据
    - 默认值：无
- **返回值（Returns）**：
  - 类型：BYTES
  - 含义：推送到主栈的数据（与输入数据类型一致，默认视为`BYTES`）
- **使用示例（Example）**：
  ```python
    # 推送字面量
    result = Push(10);  # 推送10到主栈

    # 推送固定区变量
    # 前提：固定区存在变量msg
    result = Push(msg);  # 推送msg的值到主栈
  ```

### 7. Pop
- **函数名（Function Name）**：Pop
- **描述（Short Description）**：在编译器层面从主栈或副栈移除指定变量（仅更新编译期栈状态与符号表，不生成任何字节码）。若目标是结构体变量，则会同时移除其所有字段（以 `var.` 为前缀的符号）。
- **参数（Parameters）**：
  - `variable`
    - 类型（Type）：任意类型
    - 是否可选（Optional/Required）：Required
    - 用途：需要被移除的目标变量（若为结构体变量，则连同其字段一并移除）
    - 默认值：无
- **返回值（Returns）**：
  - 类型：无返回值
  - 含义：仅执行编译期的“移除/清栈”效果，不返回具体值，也不产生操作码
- **使用示例（Example）**：
  ```python
    # 移除普通变量（从主栈或副栈查找并移除）
    a = Push(10)
    Pop(a);

    # 移除结构体变量（会递归移除字段符号，例如 person.name / person.age）
    # 前提：person 是一个结构体变量，存在 person.name、person.age 等字段
    Pop(person);
  ```

### 8. Keep
- **函数名（Function Name）**：Keep
- **描述（Short Description）**：零成本抽象函数，用于标记保留指定数量的变量。
- **参数（Parameters）**：
  - `variable1, variable2, ...`
    - 类型（Type）：任意类型（默认视为`BYTES`）
    - 是否可选（Optional/Required）：Required（至少1个）
    - 用途：需要标记保留的变量
    - 默认值：无
- **返回值（Returns）**：
  - 类型：与输入参数相同类型（默认`BYTES`）
  - 含义：返回与输入数量和类型完全一致的值，仅标记保留状态
- **使用示例（Example）**：
  ```python
    # 保留单个变量
    a = Push(10)
    Keep(a);  # 标记保留变量a

    # 保留多个变量
    x = Push(123)
    y = Push("hello")
    Keep(x, y);  # 标记保留变量x、y
  ```

---

## 下一步

- [内置对象参考](./builtin-objects.md) — `self` / `BVM` 等内置对象的完整说明

---

[🇬🇧 English version](../../en/references/builtin-functions.md)
