// runtime/statistics/CLI 共用的路径别名判定。仅供 src 内部使用，不安装。
#pragma once

#include <filesystem>
#include <string>

namespace fq::io {

/// @brief 判定两个路径是否指向同一文件系统对象
/// @details 先做归一化后的字符串比较（不要求文件存在），再对已存在的路径做
///          equivalent() 硬链接级别比较。'-'（stdout/stdin）与空路径不参与判定。
///          用于拒绝"报告/输出覆盖输入文件"这类静默数据损失配置。
inline auto pathsAlias(const std::string& lhs, const std::string& rhs) -> bool {
    if (lhs.empty() || rhs.empty()) {
        return false;
    }
    if (lhs == "-" || rhs == "-") {
        return false;
    }
    std::error_code error;
    const auto left = std::filesystem::absolute(lhs, error).lexically_normal();
    error.clear();
    const auto right = std::filesystem::absolute(rhs, error).lexically_normal();
    if (left == right) {
        return true;
    }
    error.clear();
    return std::filesystem::exists(lhs, error) && std::filesystem::exists(rhs, error) &&
        std::filesystem::equivalent(lhs, rhs, error) && !error;
}

}  // namespace fq::io
