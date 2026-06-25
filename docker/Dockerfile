# 跨平台编译Docker镜像
FROM ubuntu:22.04

# 避免交互式安装
ENV DEBIAN_FRONTEND=noninteractive

# 安装基础工具和交叉编译工具链
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    wget \
    curl \
    zip \
    unzip \
    # Windows交叉编译工具链
    gcc-mingw-w64-x86-64 \
    gcc-mingw-w64-i686 \
    # 清理缓存
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

# 设置工作目录
WORKDIR /workspace

# 复制项目文件
COPY . .

# 设置脚本执行权限
RUN chmod +x scripts/*.sh

# 默认命令：构建所有平台
CMD ["./scripts/cross-platform-builder.sh", "all", "--release"] 