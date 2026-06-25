# AtomicProof Compiler 发布指南

## 📦 发布系统概述

本项目提供了完整的版本控制和发布打包系统，可以轻松创建带版本信息的发布产物。

### 特点
- ✅ 发布包名包含版本信息（如：`Compiler-v2.0-alpha.tar.gz`）
- ✅ 解压后程序名保持简洁（`Compiler`）
- ✅ 支持多种格式（tar.gz, zip, deb）
- ✅ 自动包含文档和安装脚本
- ✅ 版本信息自动同步

## 🔧 修改版本号

### 1. 编辑 CMakeLists.txt

修改版本配置部分（第 25-35 行）：

```cmake
# 基础版本号（必须是纯数字格式）
set(PROJECT_VERSION_MAJOR 1)
set(PROJECT_VERSION_MINOR 0) 
set(PROJECT_VERSION_PATCH 0)

# 版本后缀（可选）
set(PROJECT_VERSION_SUFFIX "alpha")  # alpha, beta, rc1, 或留空表示正式版
```

### 版本配置示例

```cmake
# Alpha 版本 (1.0.0-alpha)
set(PROJECT_VERSION_MAJOR 1)
set(PROJECT_VERSION_MINOR 0)
set(PROJECT_VERSION_PATCH 0)
set(PROJECT_VERSION_SUFFIX "alpha")

# Beta 版本 (2.0.0-beta)
set(PROJECT_VERSION_MAJOR 2)
set(PROJECT_VERSION_MINOR 0)
set(PROJECT_VERSION_PATCH 0)
set(PROJECT_VERSION_SUFFIX "beta")

# RC 版本 (2.0.0-rc1)
set(PROJECT_VERSION_MAJOR 2)
set(PROJECT_VERSION_MINOR 0)
set(PROJECT_VERSION_PATCH 0)
set(PROJECT_VERSION_SUFFIX "rc1")

# 正式版本 (2.1.0)
set(PROJECT_VERSION_MAJOR 2)
set(PROJECT_VERSION_MINOR 1)
set(PROJECT_VERSION_PATCH 0)
set(PROJECT_VERSION_SUFFIX "")  # 空字符串表示正式版
```

### 2. 重新构建项目

```bash
cd build
cmake ..
make
```

## 📱 创建发布包

### 快速发布（推荐）

```bash
# 交互式发布菜单
./scripts/quick-release.sh
```

### 手动发布

```bash
# 生成 Linux 标准包
./scripts/release-packager.sh tar.gz

# 生成 ZIP 格式包
./scripts/release-packager.sh zip

# 生成 Ubuntu/Debian 包
./scripts/release-packager.sh deb

# 生成所有格式
./scripts/release-packager.sh all

# 清理并重新打包
./scripts/release-packager.sh --clean all

# 最小化包（仅可执行文件）
./scripts/release-packager.sh --minimal tar.gz
```

## 📂 发布产物

发布包保存在 `dist/` 目录：

```
dist/
├── Compiler-v1.0.0-alpha.tar.gz    # Linux 标准包
├── Compiler-v1.0.0-alpha.zip       # Linux ZIP 格式包
└── Compiler-v1.0.0-alpha.deb       # Ubuntu/Debian 包
```

⚠️ **注意：所有包格式都包含 Linux 版本的可执行文件。如需 Windows 版本，请参考 [跨平台编译指南](CROSS-PLATFORM-UNIFIED.md)。**

### 发布包内容

解压后的目录结构：

```
Compiler-v1.0.0-alpha/
├── Compiler          # Linux 可执行文件
├── doc/              # 文档目录
├── install.sh        # 安装脚本
└── VERSION           # 版本信息文件
```

## 👥 用户使用方式

### Linux/macOS 用户

```bash
# 下载并解压 tar.gz 格式
tar -xzf Compiler-v1.0.0-alpha.tar.gz
cd Compiler-v1.0.0-alpha

# 或解压 zip 格式
unzip Compiler-v1.0.0-alpha.zip
cd Compiler-v1.0.0-alpha

# 运行程序
./Compiler --version
./Compiler your-script.ct

# 可选：安装到系统
./install.sh
```

### Windows 用户

⚠️ **重要说明：** 本发布系统生成的包仅包含 Linux 版本。

**如需 Windows 版本，请：**
1. 参考 [跨平台编译指南](CROSS-PLATFORM-UNIFIED.md)
2. 或从 [Releases 页面](../../releases) 下载预编译的 Windows 版本

### Ubuntu/Debian 用户

```bash
# 直接安装 deb 包
sudo dpkg -i Compiler-v1.0.0-alpha.deb

# 运行程序
Compiler --version
```

## 🚀 发布流程示例

### 发布 Alpha 版本

```bash
# 1. 修改版本配置
vim CMakeLists.txt  # 修改版本变量
# set(PROJECT_VERSION_SUFFIX "alpha")

# 2. 构建
cd build && cmake .. && make

# 3. 打包
./scripts/quick-release.sh  # 选择格式

# 4. 分发
ls dist/  # 查看生成的文件
```

### 发布 Beta 版本

```bash
# 1. 修改版本后缀
sed -i 's/set(PROJECT_VERSION_SUFFIX "alpha")/set(PROJECT_VERSION_SUFFIX "beta")/' CMakeLists.txt

# 2. 构建并打包
cd build && cmake .. && make
./scripts/release-packager.sh all

# 3. 创建 Git 标签（可选）
git tag -a v2.0.0-beta -m "Beta release v2.0.0"
git push origin v2.0.0-beta
```

### 发布正式版本

```bash
# 1. 修改版本后缀为空（正式版）
sed -i 's/set(PROJECT_VERSION_SUFFIX "beta")/set(PROJECT_VERSION_SUFFIX "")/' CMakeLists.txt

# 2. 构建并打包
cd build && cmake .. && make
./scripts/release-packager.sh --clean all

# 3. 创建发布标签
git tag -a v2.0.0 -m "Release v2.0.0"
git push origin v2.0.0

# 4. 上传到发布平台
# 将 dist/ 目录中的文件上传到 GitHub Releases 或其他平台
```

## 📋 最佳实践

1. **版本命名规范**：
   - `x.y.z` - 正式版本
   - `x.y.z-alpha` - Alpha 测试版
   - `x.y.z-beta` - Beta 测试版
   - `x.y.z-rc1` - Release Candidate

2. **发布前检查**：
   ```bash
   # 验证版本信息
   ./build/bin/Compiler --version
   
   # 测试发布包
   cd /tmp
   tar -xzf ~/projects/compiler/dist/Compiler-v*.tar.gz
   cd Compiler-v*
   ./Compiler --version
   ```

3. **自动化建议**：
   - 在 CI/CD 中集成发布脚本
   - 使用 Git hooks 自动更新版本号
   - 设置自动化测试验证发布包

## 🔍 故障排除

### 常见问题

1. **无法找到可执行文件**
   ```bash
   # 确保先构建项目
   cd build && make
   ```

2. **版本信息不正确**
   ```bash
   # 重新构建项目
   cd build && cmake .. && make
   ```

3. **权限问题**
   ```bash
   # 给脚本添加执行权限
   chmod +x scripts/*.sh
   ```

---

🎉 **恭喜！您现在拥有了专业级的发布系统！** 