# 如何部署与调用合约

---

本文基于 `apc-buildunlockscript` 项目的实际代码，说明 APC 合约从编译、部署到调用的完整链路。这里的“部署”和“调用”都发生在 UTXO 模型里：

- **部署合约**：构造一笔交易，把 APC 编译出的 `lock.hex` 放进某个输出的 locking script。
- **调用合约**：构造一笔交易花费该合约 UTXO，并在 unlocking script 中提供合约 ABI 需要的数据。
- **状态推进**：如果合约需要校验父交易和当前交易输出，例如 `Counter`，需要用 `apc-buildunlockscript` 生成 `PreTX` / `CurrentTX` 结构体对应的解锁数据。

`AtomicProofCompiler` 负责编译 `.ct`，`apc-buildunlockscript` 负责把真实 TBC 交易序列化成合约可验证的参数。

---

## 工具分工

| 工具 | 作用 |
|------|------|
| `utxo_Interpreter` | 编译 `.ct`，输出 `lock.hex`、`abi`、`structs`、`unlock` 等 JSON 信息 |
| `tbc-lib-js` | 构造、签名和序列化 TBC 交易 |
| `tbc-contract` | 查询 UTXO / 父交易、构造 UTXO 对象、广播 raw transaction |
| `apc-buildunlockscript/src/buildTxData.ts` | 把父交易、当前交易输出序列化为 APC 合约 ABI 需要的 hex |
| `apc-buildunlockscript/src/parseAbiData.ts` | 从编译 JSON 中读取结构体数组长度、替换 `self.X` 占位符 |

`apc-buildunlockscript` 不是通用钱包。它假设你已经知道要创建哪些输出、状态如何变化，然后帮你生成合约验证所需的 unlocking script 数据。

---

## 环境准备

先把合约编译到 `build/bin`。当前 `apc-buildunlockscript/src/readFile.ts` 默认从 `/home/ubuntu/code/AtomicProofCompiler/build/bin` 读取 JSON，因此最省事的做法是在该目录生成编译结果：

```bash
cd /home/ubuntu/code/AtomicProofCompiler/build/bin
./utxo_Interpreter ../../test/contract_file/counter.ct
```

然后构建 `apc-buildunlockscript`：

```bash
cd /home/ubuntu/code/apc-buildunlockscript
npm install
npm run build
```

如果你的项目目录不同，需要同步修改 `src/readFile.ts` 中的 JSON 读取路径，或者把编译产物复制到它当前配置的目录。

---

## 编译输出如何被使用

APC 编译 JSON 中最关键的字段如下：

```json
{
  "structs": [
    {
      "name": "PreTX",
      "fields": [
        { "name": "Inputs", "type": "Input[3]" },
        { "name": "Outputs", "type": "Output[3]" }
      ]
    },
    {
      "name": "CurrentTX",
      "fields": [
        { "name": "Outputs", "type": "Output[3]" }
      ]
    }
  ],
  "abi": [
    {
      "name": "getCountFromPreTX",
      "params": [{ "name": "pretx", "type": "PreTX" }]
    },
    {
      "name": "verifyCurrentTX",
      "params": [{ "name": "ctx", "type": "CurrentTX" }]
    }
  ],
  "lock": {
    "hex": "..."
  },
  "unlock": {
    "getCountFromPreTX": "<pretx.VLIO>...",
    "verifyCurrentTX": "<ctx.Outputs[0x00].Value>..."
  }
}
```

`apc-buildunlockscript` 主要消费三类信息：

- `lock.hex`：部署交易输出的 locking script。
- `structs`：读取 `PreTX.Inputs`、`PreTX.Outputs`、`CurrentTX.Outputs` 的数组长度，决定序列化时需要填充多少个输入/输出槽位。
- `unlock` / `abi`：帮助你确认每个 public 函数需要哪些参数，以及多个 public 函数的执行顺序。

对于普通签名类合约，例如 P2PKH，unlocking script 只需要 `<sig><pubKey>`。对于 `Counter` 这类状态合约，unlocking script 需要的是当前交易输出数据和父交易数据。

---

## 部署合约

部署就是创建一个含合约脚本的 UTXO。`apc-buildunlockscript/test/buildTxData_Test.ts` 里的 `buildMintTX` 就是最小部署交易：

```ts
import * as tbc from "tbc-lib-js";
import { API } from "tbc-contract";
import { readJson } from "../src/readFile";

const network = "testnet";
const contractFileName = "counter.json";

function buildDeployTX(
  privateKey: tbc.PrivateKey,
  utxos: tbc.Transaction.IUnspentOutput[],
  lockHex: string,
  satoshis = 500,
): tbc.Transaction {
  const tx = new tbc.Transaction();

  tx.from(utxos);
  tx.addOutput(new tbc.Transaction.Output({
    script: tbc.Script.fromHex(lockHex),
    satoshis,
  }));
  tx.change(privateKey.toAddress());
  tx.feePerKb(80);
  tx.sign(privateKey);

  return tx;
}

async function deploy() {
  const privateKey = tbc.PrivateKey.fromWIF(process.env.WIF!);
  const fundingUTXO = await API.fetchUTXO(privateKey, 10, network);

  const lockHex = readJson(contractFileName).lock.hex;
  const deployTX = buildDeployTX(privateKey, [fundingUTXO], lockHex);

  console.log(deployTX.verify());
  const txid = await API.broadcastTXraw(deployTX.toString(), network);
  console.log("contract UTXO:", `${txid}:0`);
}
```

部署成功后，`txid:0` 就是合约实例。后续调用合约，本质上就是花费这个输出。

---

## 绑定或更新状态数据

如果合约脚本中有 `self.X` 或尾部状态数据，部署前要把 `lock.hex` 变成本次实例的实际脚本。

`parseAbiData.ts` 提供了占位符替换工具：

```ts
import { getLockHexWithParams } from "../src/parseAbiData";

const lockHex = getLockHexWithParams("wallet.json", {
  pubKeyHash: "2158ccfe3dc673b74e67c1ffd77842fd8bc4361c",
});
```

它会读取 JSON 中的 `constructorParams`，按类型把数值编码成小端 hex，并替换 `lock.hex` 里的 `<self.xxx>` 占位符。

`Counter` 示例没有通过 `constructorParams` 替换，而是直接改脚本尾部状态。测试代码里的做法是：找到最后一个 `OP_RETURN`，跳过 `0xff` padding，把后面的 `08 + uint64LE(count)` 当作 `SuffixData`：

```ts
function getContractDataOffset(script: tbc.Script): number {
  const scriptBuffer = script.toBuffer();
  const opReturnIndex = scriptBuffer.lastIndexOf(0x6a);

  if (opReturnIndex === -1) {
    throw new Error("OP_RETURN not found in contract script");
  }

  let dataOffset = opReturnIndex + 1;
  while (dataOffset < scriptBuffer.length && scriptBuffer[dataOffset] === 0xff) {
    dataOffset++;
  }

  return dataOffset;
}

function setCounterScriptCount(scriptHex: string, count: number): string {
  const script = tbc.Script.fromHex(scriptHex);
  const scriptBuffer = script.toBuffer();
  const dataOffset = getContractDataOffset(script);
  const countBuffer = Buffer.alloc(8);

  countBuffer.writeBigUInt64LE(BigInt(count));

  return Buffer.concat([
    scriptBuffer.subarray(0, dataOffset),
    Buffer.from([countBuffer.length]),
    countBuffer,
  ]).toString("hex");
}
```

第一次部署时可以把 `count` 设为 `1`；下一次调用时，在当前交易输出里放入 `count = 2` 的新脚本。

---

## 调用状态合约

调用 `Counter` 这类状态合约时，当前交易需要同时满足两件事：

1. 输入花费的确实是上一笔合约 UTXO。
2. 新输出里的合约状态必须按规则推进，例如 `count = oldCount + 1`。

因此 unlocking script 不能只放用户签名，还要放 `CurrentTX` 和 `PreTX` 的序列化数据。

### 生成解锁数据

`apc-buildunlockscript` 的核心调用如下：

```ts
import * as tbc from "tbc-lib-js";
import { getCurrentTxOutputsData, getPreTxdata } from "../src/buildTxData";
import { getCurrentTxArrayLengths, getPreTxArrayLengths } from "../src/parseAbiData";

const contractFileName = "counter.json";

function buildCounterUnlockScript(
  currentTX: tbc.Transaction,
  preTX: tbc.Transaction,
  contractVout: number,
): tbc.Script {
  const { numberofInputs, numberofOutputs: preTxOutputCount } =
    getPreTxArrayLengths(contractFileName);
  const { numberofOutputs: currentTxOutputCount } =
    getCurrentTxArrayLengths(contractFileName);

  const partialOffset = getContractDataOffset(preTX.outputs[contractVout].script);
  const codeLengthAndPartialOffset = new Map<number, number>([
    [preTX.outputs[contractVout].script.toBuffer().length, partialOffset],
  ]);

  const currenttxdata = getCurrentTxOutputsData(
    currentTX,
    currentTxOutputCount,
    codeLengthAndPartialOffset,
  );

  const pretxdata = getPreTxdata(
    preTX,
    contractVout,
    numberofInputs,
    preTxOutputCount,
    partialOffset,
    1,
  );

  return tbc.Script.fromHex(currenttxdata + pretxdata);
}
```

注意这里是 `currenttxdata + pretxdata`。原因是脚本执行前，unlocking script 会按写入顺序压栈，后写入的数据在栈顶。`Counter` 的 public 函数顺序是：

```text
getCountFromPreTX(pretx)
verifyCurrentTX(ctx)
```

第一个执行的 `pretx` 必须位于栈顶，所以它要拼在最后；`ctx` 先写入，留在下面，等 `getCountFromPreTX` 消费完以后再被 `verifyCurrentTX` 使用。

### 构造调用交易

下面是 `buildTxData_Test.ts` 中调用交易的精简版本：

```ts
import * as tbc from "tbc-lib-js";
import { API, buildUTXO } from "tbc-contract";
import { readJson } from "../src/readFile";

function buildCallTX(
  privateKey: tbc.PrivateKey,
  utxos: tbc.Transaction.IUnspentOutput[],
  nextLockHex: string,
  preTX: tbc.Transaction,
): tbc.Transaction {
  const tx = new tbc.Transaction();

  tx.from(utxos);
  tx.addOutput(new tbc.Transaction.Output({
    script: tbc.Script.fromHex(nextLockHex),
    satoshis: 500,
  }));
  tx.change(privateKey.toAddress());
  tx.feePerKb(80);

  tx.sign(privateKey);
  tx.setInputScript({ inputIndex: 0 }, (currentTX) => {
    return buildCounterUnlockScript(currentTX, preTX, 0);
  });
  tx.sign(privateKey);

  return tx;
}

async function callCounter() {
  const privateKey = tbc.PrivateKey.fromWIF(process.env.WIF!);
  const baseScript = readJson("counter.json").lock.hex;

  const firstScript = setCounterScriptCount(baseScript, 1);
  const secondScript = setCounterScriptCount(baseScript, 2);

  const fundingUTXO = await API.fetchUTXO(privateKey, 10, "testnet");
  const deployTX = buildDeployTX(privateKey, [fundingUTXO], firstScript);

  const callUTXOs = [
    buildUTXO(deployTX, 0),
    buildUTXO(deployTX, 1),
  ];
  const callTX = buildCallTX(privateKey, callUTXOs, secondScript, deployTX);

  console.log(callTX.verify());
  await API.broadcastTXraw(callTX.toString(), "testnet");
}
```

`setInputScript({ inputIndex: 0 }, ...)` 只覆盖合约输入的 unlocking script。交易里的普通 P2PKH 找零输入仍由 `tx.sign(privateKey)` 负责签名。

---

## `buildTxData` 参数速查

### `getPreTxdata`

```ts
getPreTxdata(
  tx,
  vout,
  numberofInputs,
  numberofOutputs,
  partialOffset,
  contractOutputNumber,
)
```

| 参数 | 含义 |
|------|------|
| `tx` | 被当前交易花费的父交易 |
| `vout` | 父交易中合约输出的位置 |
| `numberofInputs` | 合约 `PreTX.Inputs` 数组长度，不足的输入会填 `00` |
| `numberofOutputs` | 合约 `PreTX.Outputs` 数组长度，不足的输出会填空槽 |
| `partialOffset` | 合约脚本中逻辑代码和 `SuffixData` 的切分位置 |
| `contractOutputNumber` | 合约状态占用的连续输出数量；普通单 UTXO 合约填 `1` |

### `getCurrentTxOutputsData`

```ts
getCurrentTxOutputsData(
  tx,
  numberofOutputs,
  codeLengthAndPartialOffset,
)
```

| 参数 | 含义 |
|------|------|
| `tx` | 正在构造的当前交易 |
| `numberofOutputs` | 合约 `CurrentTX.Outputs` 数组长度 |
| `codeLengthAndPartialOffset` | `scriptLength -> partialOffset` 映射，用来识别当前交易中的同类合约输出 |

如果输出脚本命中 `codeLengthAndPartialOffset`，工具会按指定 `partialOffset` 拆成 `PartialHash + SuffixData`。普通短脚本会直接作为 `SuffixData`，长脚本则按 64 字节边界做 partial hash。

### 其他辅助函数

| 函数 | 用途 |
|------|------|
| `getPreTxArrayLengths(fileName)` | 从 `PreTX` 结构体读取 `Inputs` / `Outputs` 数组长度 |
| `getCurrentTxArrayLengths(fileName)` | 从 `CurrentTX` 结构体读取 `Outputs` 数组长度 |
| `getCurrentTxInputsData(tx, numberofInputs)` | 当合约显式建模当前交易输入时，序列化当前交易输入 |
| `getPrePreTxdata(...)` | 当合约需要继续追溯父交易的父交易时使用 |
| `getSize(length)` / `getLengthHex(length)` | 按 APC 合约约定编码脚本长度 |

---

## 普通签名合约的调用

如果合约不读取 `PreTX` / `CurrentTX`，调用方式更简单。以 P2PKH 为例，编译 JSON 中通常会出现：

```json
{
  "unlock": {
    "verify": "<sig><pubKey>"
  }
}
```

调用交易的输入脚本就是签名和公钥：

```ts
tx.setInputScript({ inputIndex: 0 }, () => {
  return tbc.Script.fromASM(`${sigHex} ${pubKeyHex}`);
});
```

实际项目里要确保 `sigHex` 使用和节点验证一致的 SIGHASH 类型，且签名覆盖的是当前花费交易的正确 preimage。

---

## 常见注意事项

- **不要把私钥写进测试文件**：`buildTxData_Test.ts` 里的 WIF 是本地测试写法，真实项目应从环境变量或密钥管理系统读取。
- **JSON 路径要对齐**：`readJson()` 目前使用硬编码路径，编译输出目录不一致会直接读不到文件。
- **数组长度必须来自合约 JSON**：不要手写 `3` 或 `10`；使用 `getPreTxArrayLengths()` / `getCurrentTxArrayLengths()`，否则填充槽位不一致会导致哈希校验失败。
- **`partialOffset` 必须稳定**：状态延续输出要使用和父合约输出相同的切分位置，否则 `PartialHash` 对不上。
- **多 public 函数要注意压栈顺序**：先执行的函数参数通常要拼在 unlocking script 后面，让它位于栈顶。
- **修改输出后要重新签名**：`setInputScript()` 会改变交易内容，测试代码中先签名、设置合约输入脚本、再签名一次，是为了让普通输入保持有效签名。
- **先 `tx.verify()` 再广播**：本地验证为 `true` 后，再调用 `API.broadcastTXraw(tx.toString(), network)`。
- **`codeLengthAndPartialOffset` 以脚本长度为 key**：如果同一笔交易里存在脚本长度相同但切分点不同的合约，需要扩展工具逻辑，不能只靠当前 Map。

---

## 下一步

- [如何测试合约](./how-to-test-a-contract.md) — 部署前的本地验证方案
- [教程二：Counter 合约实战](./tutorials/tutorial-2-counter.md) — 理解 `PreTX` / `CurrentTX` 状态校验

---

[English version](../en/how-to-deploy-and-call-a-contract.md)
