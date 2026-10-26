#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <istream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace fq::benchmark::detail {

/// cgroup v1 用该哨兵表示“无限额”；更大的值也按无限额处理
inline constexpr std::uint64_t kUnlimitedMemoryThreshold = std::uint64_t{1} << 60;

/// @brief 去除字符串首尾空白
inline auto trimCopy(std::string value) -> std::string {
    const auto notSpace = [](unsigned char ch) {
        return std::isspace(ch) == 0;
    };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

/// @brief 从 /proc/cpuinfo 风格文本中按字段名取值
inline auto findCpuinfoField(std::istream& stream, std::initializer_list<std::string_view> keys)
    -> std::string {
    std::string line;
    while (std::getline(stream, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        const std::string name = trimCopy(line.substr(0, colon));
        for (const std::string_view key : keys) {
            if (name == key) {
                return trimCopy(line.substr(colon + 1));
            }
        }
    }
    return {};
}

/// @brief 解析 CPU 型号；x86 用 model name，部分 ARM 平台用 Model/Hardware
inline auto parseCpuModel(std::istream& stream) -> std::string {
    return findCpuinfoField(stream, {"model name", "Model", "Hardware", "cpu model"});
}

/// @brief 解析 cgroup 内存限额；max/空/非法/无限额哨兵返回 nullopt
inline auto parseMemoryLimit(std::string_view raw) -> std::optional<std::size_t> {
    const std::string text = trimCopy(std::string(raw));
    if (text.empty() || text == "max") {
        return std::nullopt;
    }
    const bool digitsOnly = std::all_of(
        text.begin(), text.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
    if (!digitsOnly) {
        return std::nullopt;
    }

    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (end != text.c_str() + text.size()) {
        return std::nullopt;
    }
    if (parsed == 0 || parsed >= kUnlimitedMemoryThreshold) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(parsed);
}

/// @brief 解析 CPU 配额（quota/period），返回向上取整的可用核数
inline auto parseCpuQuota(std::string_view quotaRaw, std::string_view periodRaw)
    -> std::optional<int> {
    const std::string quota = trimCopy(std::string(quotaRaw));
    const std::string period = trimCopy(std::string(periodRaw));
    if (quota.empty() || quota == "max" || quota == "-1" || period.empty()) {
        return std::nullopt;
    }

    const auto digitsOnly = [](const std::string& text) {
        return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char ch) {
            return std::isdigit(ch) != 0;
        });
    };
    if (!digitsOnly(quota) || !digitsOnly(period)) {
        return std::nullopt;
    }

    const long long q = std::strtoll(quota.c_str(), nullptr, 10);
    const long long p = std::strtoll(period.c_str(), nullptr, 10);
    if (q <= 0 || p <= 0 || q > std::numeric_limits<long long>::max() - p) {
        return std::nullopt;
    }

    const long long cpus = (q + p - 1) / p;  // ceil(quota / period)
    if (cpus <= 0 || cpus > 1'000'000) {
        return std::nullopt;
    }
    return static_cast<int>(cpus);
}

/// @brief 解析 cgroup v2 的 cpu.max（格式: "<quota> <period>" 或 "max <period>"）
inline auto parseCpuMax(std::string_view raw) -> std::optional<int> {
    const std::string text = trimCopy(std::string(raw));
    const auto space = text.find(' ');
    if (space == std::string::npos) {
        return std::nullopt;
    }
    return parseCpuQuota(text.substr(0, space), text.substr(space + 1));
}

/// @brief 解析 /proc/self/cgroup 中 cgroup v2 的路径（"0::/path"）
inline auto parseCgroupV2Path(std::string_view content) -> std::optional<std::string> {
    std::size_t pos = 0;
    while (pos <= content.size()) {
        const auto end = content.find('\n', pos);
        const auto line =
            content.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        if (line.starts_with("0::")) {
            return trimCopy(std::string(line.substr(3)));
        }
        if (end == std::string_view::npos) {
            break;
        }
        pos = end + 1;
    }
    return std::nullopt;
}

/// @brief 解析 /proc/self/cgroup 中指定 v1 controller 的路径
inline auto parseCgroupV1ControllerPath(std::string_view content, std::string_view controller)
    -> std::optional<std::string> {
    std::size_t pos = 0;
    while (pos <= content.size()) {
        const auto end = content.find('\n', pos);
        const auto line =
            content.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        const auto first = line.find(':');
        if (first != std::string_view::npos) {
            const auto second = line.find(':', first + 1);
            if (second != std::string_view::npos) {
                const auto controllers = line.substr(first + 1, second - first - 1);
                std::size_t tokenStart = 0;
                while (tokenStart <= controllers.size()) {
                    const auto tokenEnd = controllers.find(',', tokenStart);
                    const auto token = controllers.substr(tokenStart,
                                                          tokenEnd == std::string_view::npos
                                                              ? std::string_view::npos
                                                              : tokenEnd - tokenStart);
                    if (token == controller) {
                        return trimCopy(std::string(line.substr(second + 1)));
                    }
                    if (tokenEnd == std::string_view::npos) {
                        break;
                    }
                    tokenStart = tokenEnd + 1;
                }
            }
        }
        if (end == std::string_view::npos) {
            break;
        }
        pos = end + 1;
    }
    return std::nullopt;
}

/// @brief 把 cgroup 相对路径拼到挂载根下
inline auto joinCgroupPath(std::string root, std::string_view relative, std::string_view leaf)
    -> std::string {
    if (!relative.empty() && relative != "/") {
        if (root.empty() || root.back() != '/') {
            root.push_back('/');
        }
        if (relative.front() == '/') {
            relative.remove_prefix(1);
        }
        root.append(relative);
    }
    if (root.empty() || root.back() != '/') {
        root.push_back('/');
    }
    root.append(leaf);
    return root;
}

}  // namespace fq::benchmark::detail
