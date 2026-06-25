#!/bin/bash

# =============================================================================
# Docker 跨平台编译脚本
# =============================================================================
# 使用Docker容器进行跨平台编译，无需安装交叉编译工具链
# =============================================================================

set -e

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

echo -e "${CYAN}=======================================${NC}"
echo -e "${CYAN}       Docker 跨平台编译${NC}"
echo -e "${CYAN}=======================================${NC}"
echo ""

# 检查Docker是否可用
if ! command -v docker &> /dev/null; then
    echo -e "${YELLOW}❌ Docker未安装！${NC}"
    echo ""
    echo "安装Docker："
    echo "  Ubuntu: sudo apt install docker.io"
    echo "  或访问: https://docs.docker.com/install/"
    exit 1
fi

# 切换到项目根目录
cd "$PROJECT_ROOT"

echo -e "${BLUE}🐳 构建Docker编译镜像...${NC}"
docker build -f docker/Dockerfile -t cross-build .

echo ""
echo -e "${BLUE}🚀 开始跨平台编译...${NC}"
docker run --rm -v "$PWD/builds:/workspace/builds" -v "$PWD/dist:/workspace/dist" cross-build

echo ""
echo -e "${BLUE}📦 打包发布产物...${NC}"
docker run --rm -v "$PWD/builds:/workspace/builds" -v "$PWD/dist:/workspace/dist" cross-build ./scripts/cross-platform-packager.sh all

echo ""
echo -e "${GREEN}✅ Docker跨平台编译完成！${NC}"
echo -e "${BLUE}📁 编译产物: builds/${NC}"
echo -e "${BLUE}📁 发布包: dist/${NC}"

# 显示结果
if [[ -d "dist" ]]; then
    echo ""
    echo "生成的发布包："
    ls -lh dist/
fi 
