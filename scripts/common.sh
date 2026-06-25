#!/bin/bash

# Common shell helpers for build and release scripts.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
CYAN='\033[0;36m'
NC='\033[0m'

PROJECT_NAME="${PROJECT_NAME:-}"
EXECUTABLE_NAME="${EXECUTABLE_NAME:-}"
PROJECT_DISPLAY_NAME="${PROJECT_DISPLAY_NAME:-}"
PROJECT_VERSION="${PROJECT_VERSION:-}"

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

log_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_step() {
    echo -e "${PURPLE}[STEP]${NC} $1"
}

parse_project_info() {
    log_step "解析项目信息..."

    local cmake_file="$PROJECT_ROOT/CMakeLists.txt"
    if [[ ! -f "$cmake_file" ]]; then
        log_error "CMakeLists.txt 不存在: $cmake_file"
        exit 1
    fi

    PROJECT_NAME=$(grep -E "^project\s*\(" "$cmake_file" | sed -E 's/project\s*\(\s*([a-zA-Z_][a-zA-Z0-9_]*).*/\1/' | head -1)
    if [[ -z "$PROJECT_NAME" ]]; then
        log_error "无法从 CMakeLists.txt 解析项目名称"
        exit 1
    fi

    local custom_executable
    custom_executable=$(grep -E "set\s*\(\s*EXECUTABLE_NAME" "$cmake_file" | sed -E 's/.*EXECUTABLE_NAME\s+"([^"]+)".*/\1/' | head -1)
    if [[ -n "$custom_executable" && "$custom_executable" != "\${PROJECT_NAME}" ]]; then
        EXECUTABLE_NAME="$custom_executable"
    else
        EXECUTABLE_NAME="$PROJECT_NAME"
    fi

    PROJECT_DISPLAY_NAME=$(grep -E "DESCRIPTION\s+" "$cmake_file" | sed -E 's/.*DESCRIPTION\s+"([^"]+)".*/\1/' | head -1)
    if [[ -z "$PROJECT_DISPLAY_NAME" ]]; then
        PROJECT_DISPLAY_NAME="$PROJECT_NAME"
    fi

    log_info "项目名称: $PROJECT_NAME"
    log_info "可执行文件名: $EXECUTABLE_NAME"
    log_info "显示名称: $PROJECT_DISPLAY_NAME"
}
