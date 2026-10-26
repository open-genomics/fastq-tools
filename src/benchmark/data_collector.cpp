#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE  // sched_getaffinity / CPU_COUNT
#endif

#include "data_collector.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

#include "data_collector_detail.h"

#ifdef __linux__
#include <sched.h>
#include <unistd.h>

#include <sys/sysinfo.h>
#endif

namespace fq::benchmark {

namespace {

/**
 * @brief 读取单行文件内容（去除 NUL 与首尾空白）
 *
 * ARM 服务器的 /proc/device-tree/model 是 NUL 结尾的二进制属性，
 * 不能按普通文本直接使用。
 */
std::string readFirstLine(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::string line;
    if (!std::getline(input, line)) {
        return {};
    }
    line.erase(std::remove(line.begin(), line.end(), '\0'), line.end());
    return detail::trimCopy(std::move(line));
}

/**
 * @brief 读取整个文件内容
 */
std::string readFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

/**
 * @brief 读取进程自身 cgroup v2 控制文件
 */
std::string readCgroupV2File(std::string_view leaf) {
    const auto relative = detail::parseCgroupV2Path(readFile("/proc/self/cgroup"));
    if (!relative) {
        return {};
    }
    return readFirstLine(detail::joinCgroupPath("/sys/fs/cgroup", *relative, leaf).c_str());
}

/**
 * @brief 读取进程自身 cgroup v1 controller 控制文件
 */
std::string readCgroupV1File(std::string_view controller, std::string_view leaf) {
    const auto relative =
        detail::parseCgroupV1ControllerPath(readFile("/proc/self/cgroup"), controller);
    if (!relative) {
        return {};
    }
    const std::string root = "/sys/fs/cgroup/" + std::string(controller);
    return readFirstLine(detail::joinCgroupPath(root, *relative, leaf).c_str());
}

std::optional<std::size_t> readMemoryLimit() {
    if (const auto value = detail::parseMemoryLimit(readCgroupV2File("memory.max"))) {
        return value;
    }
    if (const auto value =
            detail::parseMemoryLimit(readCgroupV1File("memory", "memory.limit_in_bytes"))) {
        return value;
    }
    // 兜底：/proc/self/cgroup 不可读或旧布局
    if (const auto value = detail::parseMemoryLimit(readFirstLine("/sys/fs/cgroup/memory.max"))) {
        return value;
    }
    return detail::parseMemoryLimit(readFirstLine("/sys/fs/cgroup/memory/memory.limit_in_bytes"));
}

std::optional<int> readCpuQuota() {
    if (const auto value = detail::parseCpuMax(readCgroupV2File("cpu.max"))) {
        return value;
    }
    if (const auto value = detail::parseCpuQuota(readCgroupV1File("cpu", "cpu.cfs_quota_us"),
                                                 readCgroupV1File("cpu", "cpu.cfs_period_us"))) {
        return value;
    }
    if (const auto value = detail::parseCpuMax(readFirstLine("/sys/fs/cgroup/cpu.max"))) {
        return value;
    }
    return detail::parseCpuQuota(readFirstLine("/sys/fs/cgroup/cpu/cpu.cfs_quota_us"),
                                 readFirstLine("/sys/fs/cgroup/cpu/cpu.cfs_period_us"));
}

bool isUsableCpuModel(const std::string& model) {
    return !model.empty() && model != "unknown" && model != "-" && model != "N/A" &&
        model != "Not Specified";
}

/**
 * @brief 执行命令并获取输出
 */
std::string execCommand(const char* cmd) {
    std::array<char, 256> buffer{};
    std::string result;

    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) {
        return "unknown";
    }

    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe.get()) != nullptr) {
        result += buffer.data();
    }

    // 移除尾部换行符
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }

    return result.empty() ? "unknown" : result;
}

}  // namespace

SystemMetadata DataCollector::collectSystemMetadata() {
    SystemMetadata metadata;
    metadata.timestamp = getTimestamp();
    metadata.gitCommit = getGitCommit();
    metadata.gitBranch = getGitBranch();
    metadata.cpuModel = getCpuModel();
    metadata.coreCount = getCoreCount();
    metadata.memoryBytes = getMemoryBytes();
    metadata.osVersion = getOsVersion();
    metadata.compilerVersion = getCompilerVersion();
    return metadata;
}

std::string DataCollector::getGitCommit() {
    return execCommand("git rev-parse --short HEAD 2>/dev/null");
}

std::string DataCollector::getGitBranch() {
    return execCommand("git rev-parse --abbrev-ref HEAD 2>/dev/null");
}

std::string DataCollector::getCpuModel() {
#ifdef __linux__
    // x86 与部分 ARM 平台直接提供 model name；ARM 常见字段是 Model/Hardware
    {
        std::ifstream cpuinfo("/proc/cpuinfo");
        if (cpuinfo) {
            auto model = detail::parseCpuModel(cpuinfo);
            if (!model.empty()) {
                return model;
            }
        }
    }

    // 固定 C locale：lscpu 的字段标签会被 gettext 本地化，
    // LC_ALL=C + 空 LANGUAGE 保证 "Model name:" 可匹配。
    // ARM 上这是把 MIDR 解码成 Kunpeng-920 等型号的唯一稳定来源。
    const std::string lscpuModel = execCommand(
        "LC_ALL=C LANGUAGE= lscpu 2>/dev/null | sed -n 's/^Model name:[[:space:]]*//p' | head -n "
        "1");
    if (isUsableCpuModel(lscpuModel)) {
        return lscpuModel;
    }

    // CPU 型号不可得时，退到板卡/整机型号（不同内核对 device tree 的挂载路径不同）
    for (const char* path : {"/proc/device-tree/model",
                             "/sys/firmware/devicetree/base/model",
                             "/sys/devices/virtual/dmi/id/product_name"}) {
        const std::string model = readFirstLine(path);
        if (!model.empty()) {
            return model;
        }
    }
#endif
    return execCommand("uname -p 2>/dev/null");
}

int DataCollector::getCoreCount() {
#ifdef __linux__
    int affinityCpus = 0;
    cpu_set_t cpuSet;
    CPU_ZERO(&cpuSet);
    if (sched_getaffinity(0, sizeof(cpuSet), &cpuSet) == 0) {
        affinityCpus = CPU_COUNT(&cpuSet);
    }

    // CFS 配额（cpu.max）不改变 affinity；benchmark 口径取二者较小值
    const auto quota = readCpuQuota();
    if (affinityCpus > 0) {
        if (quota && *quota < affinityCpus) {
            return *quota;
        }
        return affinityCpus;
    }
    if (quota) {
        return *quota;
    }
#endif
    return static_cast<int>(std::thread::hardware_concurrency());
}

std::size_t DataCollector::getMemoryBytes() {
#ifdef __linux__
    std::size_t hostBytes = 0;
    struct sysinfo info{};
    if (sysinfo(&info) == 0) {
        hostBytes = static_cast<std::size_t>(info.totalram) * info.mem_unit;
    }

    // 容器内 sysinfo 报宿主内存；cgroup 限额才是进程实际可用预算。
    if (const auto limit = readMemoryLimit()) {
        if (hostBytes == 0 || *limit < hostBytes) {
            return *limit;
        }
    }
    return hostBytes;
#else
    return 0;
#endif
}

std::string DataCollector::getOsVersion() {
#ifdef __linux__
    // 尝试读取 /etc/os-release
    std::ifstream osRelease("/etc/os-release");
    std::string line;
    while (std::getline(osRelease, line)) {
        if (line.starts_with("PRETTY_NAME=")) {
            std::string name = line.substr(12);
            // 移除引号
            if (!name.empty() && name.front() == '"') {
                name.erase(0, 1);
            }
            if (!name.empty() && name.back() == '"') {
                name.pop_back();
            }
            return name;
        }
    }
#endif
    return execCommand("uname -sr 2>/dev/null");
}

std::string DataCollector::getCompilerVersion() {
#if defined(__clang__)
    return "clang " + std::to_string(__clang_major__) + "." + std::to_string(__clang_minor__) +
        "." + std::to_string(__clang_patchlevel__);
#elif defined(__GNUC__)
    return "gcc " + std::to_string(__GNUC__) + "." + std::to_string(__GNUC_MINOR__) + "." +
        std::to_string(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    return "msvc " + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

std::string DataCollector::getTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::string DataCollector::toJson(const BenchmarkReport& report) {
    nlohmann::json j = report;
    return j.dump(2);
}

BenchmarkReport DataCollector::fromJson(const std::string& json) {
    auto j = nlohmann::json::parse(json);
    return j.get<BenchmarkReport>();
}

std::string DataCollector::generateResultFilename(const SystemMetadata& metadata) {
    // 从 ISO 时间戳提取日期时间部分
    std::string ts = metadata.timestamp;
    // 替换 T 和 : 为 _ 和 -
    for (char& c : ts) {
        if (c == 'T') {
            c = '_';
        }
        if (c == ':') {
            c = '-';
        }
    }
    // 移除 Z 后缀
    if (!ts.empty() && ts.back() == 'Z') {
        ts.pop_back();
    }

    return ts + "_" + metadata.gitCommit + ".json";
}

}  // namespace fq::benchmark
