#!/bin/bash
# scripts/lib/common.sh
# 
# 公共函数库 - 为所有 FastQTools 脚本提供统一的工具函数
# 
# 使用方法:
#   source "$(dirname "${BASH_SOURCE[0]}")/lib/common.sh"

# =============================================================================
# 颜色和样式定义
# =============================================================================
readonly COLOR_RED='\033[0;31m'
readonly COLOR_GREEN='\033[0;32m'
readonly COLOR_YELLOW='\033[1;33m'
readonly COLOR_BLUE='\033[0;34m'
readonly COLOR_MAGENTA='\033[0;35m'
readonly COLOR_CYAN='\033[0;36m'
readonly COLOR_BOLD='\033[1m'
readonly COLOR_RESET='\033[0m'

# =============================================================================
# 日志函数
# =============================================================================

# 信息日志
log_info() {
    echo -e "${COLOR_BLUE}[INFO]${COLOR_RESET} $*"
}

# 成功日志
log_success() {
    echo -e "${COLOR_GREEN}[SUCCESS]${COLOR_RESET} $*"
}

# 警告日志
log_warning() {
    echo -e "${COLOR_YELLOW}[WARNING]${COLOR_RESET} $*" >&2
}

# 错误日志
log_error() {
    echo -e "${COLOR_RED}[ERROR]${COLOR_RESET} $*" >&2
}

# 调试日志（需要 VERBOSE=true）
log_debug() {
    if [[ "${VERBOSE:-false}" == "true" ]]; then
        echo -e "${COLOR_CYAN}[DEBUG]${COLOR_RESET} $*" >&2
    fi
}

# 分隔线
log_separator() {
    local char="${1:--}"
    local width="${2:-80}"
    printf '%*s\n' "$width" '' | tr ' ' "$char"
}

# 带标题的分隔线
log_section() {
    local title="$1"
    log_separator "="
    echo -e "${COLOR_BOLD}${title}${COLOR_RESET}"
    log_separator "="
}

# =============================================================================
# 错误处理
# =============================================================================
# 各脚本自行 `set -euo pipefail`，无需统一 trap
# =============================================================================
# 路径和环境工具
# =============================================================================

# 获取项目根目录
get_project_root() {
    local script_dir
    script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    echo "$(dirname "$script_dir")"
}

# 标准化 Sanitizer 名称
normalize_sanitizer() {
    local sanitizer="${1:-}"

    case "${sanitizer,,}" in
        "")
            echo ""
            ;;
        asan|address)
            echo "asan"
            ;;
        tsan|thread)
            echo "tsan"
            ;;
        ubsan|undefined)
            echo "ubsan"
            ;;
        msan|memory)
            echo "msan"
            ;;
        *)
            log_error "Unknown sanitizer: ${sanitizer}"
            return 1
            ;;
    esac
}

# 根据脚本选项推导默认的 CMake preset（若存在）
get_default_cmake_preset() {
    local compiler="${1:-clang}"
    local build_type="${2:-Release}"
    local sanitizer="${3:-}"
    local coverage="${4:-false}"

    # Linux ARM64 不套用预设：clang 预设强制 -stdlib=libc++，而 ARM 服务器
    # 通常使用系统 libstdc++（Conan 依赖也按 libstdc++ 构建）。返回非零让
    # scripts/core/build 走显式 CMake 参数路径，保持 ABI 一致。
    if [[ "$(detect_os)" == "linux" && "$(uname -m 2>/dev/null)" =~ ^(aarch64|arm64)$ ]]; then
        return 1
    fi

    if [[ "${coverage}" == "true" ]]; then
        if [[ "${compiler}" == "gcc" ]]; then
            echo "coverage"
            return 0
        fi
        return 1
    fi

    if [[ -n "${sanitizer}" ]]; then
        if [[ "${compiler}" == "clang" ]]; then
            case "${sanitizer}" in
                asan|tsan|ubsan|msan)
                    echo "clang-${sanitizer}"
                    return 0
                    ;;
            esac
        fi
        return 1
    fi

    case "${build_type}" in
        Debug|Release|RelWithDebInfo)
            echo "${compiler}-$(echo "${build_type}" | tr '[:upper:]' '[:lower:]')"
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

# 根据主机架构与编译器返回 Conan host profile 路径
# （Conan 用 armv8 表示 64 位 ARM）。未知架构返回非零。
get_conan_profile() {
    local compiler="${1:-clang}"
    local os arch
    os="$(detect_os)"
    arch="$(uname -m 2>/dev/null || echo unknown)"

    # 仓库内 profile 均为 os=Linux；macOS（含 Apple Silicon）交给 Conan
    # 默认 profile，避免把 Linux 依赖图套到 macOS 上。
    if [[ "${os}" != "linux" ]]; then
        return 1
    fi

    case "${arch}" in
        x86_64|amd64)
            echo "build-config/conan/profile-${compiler}"
            ;;
        aarch64|arm64)
            echo "build-config/conan/profile-${compiler}-arm64"
            ;;
        *)
            return 1
            ;;
    esac
}

# 将 pip --user 的脚本目录加入当前 PATH（存在且尚未包含时）。
# install-deps 无法修改父 shell 环境，build/doctor 需自行探测，否则刚装好的
# conan 在下一个命令里仍然 “not found”。Linux 为 ~/.local/bin，
# macOS 为 ~/Library/Python/X.Y/bin，用 site --user-base 统一推导。
ensure_pip_user_bin_in_path() {
    local conan_major
    # 已有可用的 Conan 2 时不改 PATH，避免用户目录里的其他工具遮蔽系统工具
    if command -v conan >/dev/null 2>&1; then
        conan_major="$(conan --version 2>/dev/null | grep -oE '[0-9]+' | head -1 || true)"
        if [[ "$conan_major" == "2" ]]; then
            return 0
        fi
    fi

    local user_base user_bin
    command -v python3 >/dev/null 2>&1 || return 0
    user_base="$(python3 -m site --user-base 2>/dev/null || true)"
    [[ -n "$user_base" ]] || return 0
    user_bin="${user_base}/bin"
    # 只有该目录确实含 conan 时才前置，缩小对其他工具的遮蔽面
    [[ -x "${user_bin}/conan" ]] || return 0
    case ":${PATH}:" in
        *":${user_bin}:"*) ;;
        *)
            export PATH="${user_bin}:${PATH}"
            hash -r 2>/dev/null || true
            ;;
    esac
    return 0
}

# 获取构建目录名称（与 CMakePresets.json binaryDir 一致）
get_build_dir() {
    local compiler="${1:-clang}"
    local build_type="${2:-Release}"
    local sanitizer="${3:-}"
    local coverage="${4:-false}"
    local preset="${5:-}"
    local build_type_lower

    if [[ -n "${preset}" ]]; then
        echo "build/${preset}"
        return 0
    fi

    if [[ "${coverage}" == "true" ]]; then
        echo "build/coverage"
        return 0
    fi

    if [[ -n "${sanitizer}" ]]; then
        echo "build/${compiler}-${sanitizer}"
        return 0
    fi

    build_type_lower=$(echo "$build_type" | tr '[:upper:]' '[:lower:]')
    echo "build/${compiler}-${build_type_lower}"
}

# 查找可用的编译数据库目录
find_compile_commands_dir() {
    local preferred_dir="${1:-}"
    local candidate=""
    local candidates=(
        build/clang-debug
        build/clang-release
        build/clang-asan
        build/clang-tsan
        build/clang-ubsan
        build/clang-msan
        build/gcc-debug
        build/gcc-release
        build/gcc-relwithdebinfo
        build/coverage
    )

    if [[ -n "${preferred_dir}" && -f "${preferred_dir}/compile_commands.json" ]]; then
        echo "${preferred_dir}"
        return 0
    fi

    for candidate in "${candidates[@]}"; do
        if [[ -f "${candidate}/compile_commands.json" ]]; then
            echo "${candidate}"
            return 0
        fi
    done

    if [[ -d build ]]; then
        while IFS= read -r candidate; do
            if [[ -n "${candidate}" ]]; then
                echo "${candidate}"
                return 0
            fi
        done < <(find build -mindepth 1 -maxdepth 2 -type f -name "compile_commands.json" -printf '%h\n' 2>/dev/null | sort)
    fi

    return 1
}

# 检查命令是否存在
require_command() {
    local cmd="$1"
    local install_hint="${2:-}"
    
    if ! command -v "$cmd" &>/dev/null; then
        log_error "Required command not found: ${cmd}"
        if [[ -n "$install_hint" ]]; then
            log_info "Install hint: ${install_hint}"
        fi
        return 1
    fi
    return 0
}

# 检查多个命令
require_commands() {
    local missing=()
    local cmd
    
    for cmd in "$@"; do
        if ! command -v "$cmd" &>/dev/null; then
            missing+=("$cmd")
        fi
    done
    
    if [[ ${#missing[@]} -gt 0 ]]; then
        log_error "Missing required commands: ${missing[*]}"
        return 1
    fi
    return 0
}

# =============================================================================
# 文件和目录工具
# =============================================================================

# 确保目录存在
ensure_directory() {
    local dir="$1"
    if [[ ! -d "$dir" ]]; then
        log_debug "Creating directory: ${dir}"
        mkdir -p "$dir"
    fi
}

# 安全地删除目录
safe_remove_dir() {
    local dir="$1"
    if [[ -d "$dir" ]]; then
        log_debug "Removing directory: ${dir}"
        rm -rf "$dir"
    fi
}

# 检查文件是否存在且可执行
check_executable() {
    local file="$1"
    if [[ ! -x "$file" ]]; then
        log_error "Executable not found or not executable: ${file}"
        return 1
    fi
    return 0
}

# =============================================================================
# 参数解析工具
# =============================================================================

# 显示帮助信息的标准格式
print_usage_header() {
    local script_name="$1"
    local description="$2"
    
    log_section "$script_name"
    echo "$description"
    echo ""
    echo "Usage: $0 [OPTIONS]"
    echo ""
}

# =============================================================================
# 构建工具
# =============================================================================

# 获取 CPU 核心数
get_cpu_cores() {
    if command -v nproc &>/dev/null; then
        nproc
    elif command -v sysctl &>/dev/null; then
        sysctl -n hw.ncpu
    else
        echo "4"  # 默认值
    fi
}

# 标准化构建类型
normalize_build_type() {
    local build_type="$1"
    case "${build_type,,}" in
        debug|dbg|d)
            echo "Debug"
            ;;
        release|rel|r)
            echo "Release"
            ;;
        relwithdebinfo|relwithdeb|rwdi)
            echo "RelWithDebInfo"
            ;;
        coverage|cov)
            echo "Coverage"
            ;;
        sanitize|san|asan)
            echo "Sanitize"
            ;;
        *)
            log_error "Unknown build type: ${build_type}"
            return 1
            ;;
    esac
}

# 标准化编译器名称
normalize_compiler() {
    local compiler="$1"
    case "${compiler,,}" in
        gcc|gnu)
            echo "gcc"
            ;;
        clang|llvm)
            echo "clang"
            ;;
        *)
            log_error "Unknown compiler: ${compiler}"
            return 1
            ;;
    esac
}

# =============================================================================
# 进度显示
# =============================================================================

# 显示进度步骤
show_step() {
    local current="$1"
    local total="$2"
    local description="$3"
    
    echo ""
    log_info "[${current}/${total}] ${description}"
}

# =============================================================================
# 时间测量
# =============================================================================

# 开始计时
start_timer() {
    TIMER_START=$(date +%s)
}

# 结束计时并显示
end_timer() {
    local timer_end
    timer_end=$(date +%s)
    local duration=$((timer_end - TIMER_START))
    local minutes=$((duration / 60))
    local seconds=$((duration % 60))
    
    if [[ $minutes -gt 0 ]]; then
        log_success "Completed in ${minutes}m ${seconds}s"
    else
        log_success "Completed in ${seconds}s"
    fi
}

# =============================================================================
# 系统检测
# =============================================================================

# 检测操作系统
detect_os() {
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        echo "linux"
    elif [[ "$OSTYPE" == "darwin"* ]]; then
        echo "macos"
    elif [[ "$OSTYPE" == "msys" ]] || [[ "$OSTYPE" == "win32" ]]; then
        echo "windows"
    else
        echo "unknown"
    fi
}

# 检测发行版（仅 Linux）
detect_distro() {
    if [[ -f /etc/os-release ]]; then
        source /etc/os-release
        echo "${ID:-unknown}"
    else
        echo "unknown"
    fi
}

# =============================================================================
# 确认提示
# =============================================================================

# 请求用户确认
confirm() {
    local prompt="${1:-Continue?}"
    local default="${2:-n}"
    
    if [[ "$default" == "y" ]]; then
        read -p "${prompt} [Y/n]: " -n 1 -r
    else
        read -p "${prompt} [y/N]: " -n 1 -r
    fi
    echo
    
    if [[ "$default" == "y" ]]; then
        [[ $REPLY =~ ^[Nn]$ ]] && return 1
    else
        [[ ! $REPLY =~ ^[Yy]$ ]] && return 1
    fi
    return 0
}

# =============================================================================
# 版本比较
# =============================================================================

# 比较版本号（返回 0 如果 v1 >= v2）
version_ge() {
    local v1="$1"
    local v2="$2"
    
    # 使用 sort -V 进行版本比较
    if printf '%s\n' "$v2" "$v1" | sort -V -C; then
        return 0
    else
        return 1
    fi
}

# =============================================================================
# 工具检查函数
# =============================================================================

# 检查工具是否存在并提供安装提示
check_tool() {
    local tool="$1"
    local install_hint="${2:-}"
    
    if ! command -v "$tool" &>/dev/null; then
        log_warning "$tool 未找到"
        if [[ -n "$install_hint" ]]; then
            log_info "安装提示: $install_hint"
        else
            # 提供默认安装提示
            case "$tool" in
                valgrind)
                    log_info "安装提示: sudo apt-get install valgrind / brew install valgrind"
                    ;;
                cppcheck)
                    log_info "安装提示: sudo apt-get install cppcheck / brew install cppcheck"
                    ;;
                include-what-you-use|iwyu)
                    log_info "安装提示: sudo apt-get install iwyu / brew install include-what-you-use"
                    ;;
                clang-format)
                    log_info "安装提示: sudo apt-get install clang-format / brew install clang-format"
                    ;;
                clang-tidy)
                    log_info "安装提示: sudo apt-get install clang-tidy / brew install llvm"
                    ;;
                lcov|genhtml)
                    log_info "安装提示: sudo apt-get install lcov / brew install lcov"
                    ;;
                *)
                    log_info "请安装 $tool 后重试"
                    ;;
            esac
        fi
        return 1
    fi
    return 0
}

# 检查多个工具并返回缺失列表
check_tools_list() {
    local missing=()
    local tool
    
    for tool in "$@"; do
        if ! command -v "$tool" &>/dev/null; then
            missing+=("$tool")
        fi
    done
    
    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "${missing[*]}"
        return 1
    fi
    return 0
}

# 获取工具版本
get_tool_version() {
    local tool="$1"
    local version=""
    
    case "$tool" in
        gcc|g++)
            version=$("$tool" --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
            ;;
        clang|clang++)
            version=$("$tool" --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)
            ;;
        cmake)
            version=$(cmake --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
            ;;
        valgrind)
            version=$(valgrind --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+')
            ;;
        cppcheck)
            version=$(cppcheck --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+')
            ;;
        *)
            version=$("$tool" --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1)
            ;;
    esac
    
    echo "$version"
}

# =============================================================================
# 导出函数
# =============================================================================

# 导出所有公共函数供子 shell 使用
export -f log_info log_success log_warning log_error log_debug
export -f log_separator log_section
export -f get_project_root get_build_dir get_conan_profile get_default_cmake_preset find_compile_commands_dir
export -f ensure_pip_user_bin_in_path
export -f require_command require_commands
export -f ensure_directory safe_remove_dir check_executable
export -f get_cpu_cores normalize_build_type normalize_compiler normalize_sanitizer
export -f show_step start_timer end_timer
export -f detect_os detect_distro confirm version_ge
export -f check_tool check_tools_list get_tool_version
