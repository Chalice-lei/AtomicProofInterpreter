#!/bin/bash

# =============================================================================
# 快速发布脚本
# =============================================================================
# 简化的发布流程，一键生成常用格式的发布包
# =============================================================================

set -e

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

RELEASE_PACKAGER="$SCRIPT_DIR/release-packager.sh"

echo -e "${CYAN}=======================================${NC}"
echo -e "${CYAN}       快速发布菜单${NC}"
echo -e "${CYAN}=======================================${NC}"
echo ""

echo -e "${YELLOW}选择发布格式:${NC}"
echo ""
echo -e "  ${GREEN}1)${NC} Linux 标准包      (tar.gz)"
echo -e "  ${GREEN}2)${NC} 跨平台压缩包      (zip)"  
echo -e "  ${GREEN}3)${NC} Ubuntu/Debian包   (deb)"
echo -e "  ${GREEN}4)${NC} 全格式发布        (tar.gz + zip + deb)"
echo -e "  ${GREEN}5)${NC} 最小化包          (仅可执行文件)"
echo -e "  ${GREEN}6)${NC} 清理并重新打包    (清理旧文件)"
echo ""

read -p "请选择 (1-6): " choice

case $choice in
    1)
        echo -e "${BLUE}生成 Linux 标准包...${NC}"
        "$RELEASE_PACKAGER" tar.gz
        ;;
    2)
        echo -e "${BLUE}生成跨平台压缩包...${NC}"
        "$RELEASE_PACKAGER" zip
        ;;
    3)
        echo -e "${BLUE}生成 Ubuntu/Debian 包...${NC}"
        "$RELEASE_PACKAGER" deb
        ;;
    4)
        echo -e "${BLUE}生成全格式发布包...${NC}"
        "$RELEASE_PACKAGER" all
        ;;
    5)
        echo -e "${BLUE}生成最小化包...${NC}"
        "$RELEASE_PACKAGER" --minimal tar.gz
        ;;
    6)
        echo -e "${BLUE}清理并重新打包...${NC}"
        "$RELEASE_PACKAGER" --clean all
        ;;
    *)
        echo "无效选择"
        exit 1
        ;;
esac

echo ""
echo -e "${GREEN}✨ 发布完成！${NC}"
echo ""
echo -e "${YELLOW}下一步:${NC}"
echo "  1. 检查 dist/ 目录中的发布文件"
echo "  2. 测试发布包是否正常工作"
echo "  3. 上传到发布平台或分发给用户" 
