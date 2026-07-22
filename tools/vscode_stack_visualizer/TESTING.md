# AtomicProof Stack Visualizer 自动化测试

## 套件与依赖

| 命令 | 主要覆盖 | 必需环境 |
| --- | --- | --- |
| `npm run test:fast` | Schema、5 个现有 Trace、elementId、8 个命令、Trace DAP、伪 Live DAP、auto save、安全 | Node.js |
| `npm run test:webview` | 离线 Webview、导航/播放、书签、生命周期、复制、诊断、XSS、视觉、axe | Chromium |
| `npm run test:live` | 真实 JSONL debug-server、真实 Live Adapter、错误启动与进程退出 | `build/bin/utxo_Interpreter` |
| `npm run test:extension` | 真实 Extension Host、公开命令、Trace DAP、trace/live 保存重启 | VS Code Electron；Linux CI 使用 Xvfb |
| `npm run test:package` | VSIX 内容白名单、manifest、CLI 安装、干净 Extension Host 激活 | VS Code Electron；Linux CI 使用 Xvfb |
| `npm run test:performance` | 10,000 step、1,000 元素栈、DOM 虚拟化、堆增长、连续播放 | Chromium |
| `npm run test:full` | 除 nightly 性能外的全部核心套件 | 上述全部核心依赖 |

核心命令不把解释器或浏览器缺失当作 skip。所有自行启动的子进程都有超时；
超时后先发 `SIGTERM`，再升级为 `SIGKILL`，Extension Host/VSIX 测试还会清理
整个进程组。

## 覆盖矩阵

| 领域 | 自动化证据 |
| --- | --- |
| 8 个命令 | 逐个验证注册、正常、取消/无上下文和错误路径；真实 Host 复验打开、调试、开关和重启 |
| Trace 输入 | Draft 2020-12 Schema 真校验；非法 JSON、错误 format、旧格式、缺字段、错类型、空/单步 |
| 栈呈现 | Main/Alt Before/After/Diff；push/pop/main↔alt move/reorder |
| Webview 导航 | Prev/Next、时间轴、连续播放、速度、搜索、过滤、事件、函数、源码消息、键盘 |
| 书签 | localStorage 持久化、同 Trace 恢复、不同 Trace 隔离、事件轨标记 |
| elementId | 5 个样例的生产/移动/消费不变量；Webview 生命周期选择和跳转 |
| 复制/诊断 | Markdown、JSON 实际剪贴板内容；完整、旧、不完整、空 Trace 诊断 |
| Trace DAP | next/stepBack/continue/reverseContinue、普通/条件/hit count/logpoint 断点、variables/evaluate/source |
| Live DAP | launch、breakpoint、continue/next/stepIn/stepOut、variables/evaluate、disconnect/terminate、异常退出 |
| 保存刷新 | trace/live、逐合约 debounce、两个合约隔离、真实 Extension Host 重启 |
| 安全 | Webview XSS、argv 命令注入、缺失/越界源码路径与显式确认、nonce CSP、VSIX 内容白名单 |
| 性能 | 10,000 step 首屏/跳转门槛、1,000 元素虚拟栈、DOM 上限、堆增长、timer 清理 |
| 可访问性/视觉 | 键盘和 tab ARIA、axe WCAG 2 A/AA serious/critical 为零、亮/暗/窄屏截图 |
| 打包 | 精确文件白名单、manifest/贡献点、临时 extensions 目录安装、非源码目录激活 |

测试数据包含 `examples/stack_traces` 的 5 个文件，并在运行时生成边界值、恶意
HTML、缺字段、空/单步、10,000 step 和 1,000 元素栈，避免把大型生成文件提交
到仓库。

## CI

`.github/workflows/stack-visualizer-check.yml` 在 PR/推送中构建带 debugger 的解释器，
安装 Chromium，并在 Xvfb 下运行 `test:full`。nightly/手工任务在 Linux、Windows、
macOS 运行核心套件，并在 Linux 单独运行 `test:performance`。
