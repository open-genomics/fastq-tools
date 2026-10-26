#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include "benchmark/data_collector.h"
#include "benchmark/data_collector_detail.h"
#include <gtest/gtest.h>

namespace {

// --- 纯解析函数：确定性用例 ---

TEST(DataCollectorDetailTest, TrimCopy_RemovesLeadingAndTrailingWhitespace) {
    EXPECT_EQ(fq::benchmark::detail::trimCopy("  a b \t\r\n"), "a b");
    EXPECT_EQ(fq::benchmark::detail::trimCopy(""), "");
    EXPECT_EQ(fq::benchmark::detail::trimCopy(" \t\n"), "");
}

TEST(DataCollectorDetailTest, ParseCpuModel_X86ModelName) {
    std::istringstream cpuinfo(
        "processor\t: 0\n"
        "model name\t: AMD Ryzen 7 5800H with Radeon Graphics\n"
        "cpu MHz\t\t: 3200.000\n");
    EXPECT_EQ(fq::benchmark::detail::parseCpuModel(cpuinfo),
              "AMD Ryzen 7 5800H with Radeon Graphics");
}

TEST(DataCollectorDetailTest, ParseCpuModel_ArmModelOrHardware) {
    std::istringstream model("processor\t: 0\nModel\t\t: Kunpeng-920\n");
    EXPECT_EQ(fq::benchmark::detail::parseCpuModel(model), "Kunpeng-920");

    std::istringstream hardware("processor\t: 0\nHardware\t: BCM2835\n");
    EXPECT_EQ(fq::benchmark::detail::parseCpuModel(hardware), "BCM2835");
}

TEST(DataCollectorDetailTest, ParseCpuModel_NoKnownFieldReturnsEmpty) {
    std::istringstream cpuinfo("processor\t: 0\nCPU part\t: 0xd01\n");
    EXPECT_TRUE(fq::benchmark::detail::parseCpuModel(cpuinfo).empty());
}

TEST(DataCollectorDetailTest, ParseMemoryLimit_AcceptsFiniteBytes) {
    const auto large = fq::benchmark::detail::parseMemoryLimit("214748364800");
    ASSERT_TRUE(large.has_value());
    EXPECT_EQ(*large, std::size_t{214748364800ULL});

    const auto padded = fq::benchmark::detail::parseMemoryLimit(" 1024 \n");
    ASSERT_TRUE(padded.has_value());
    EXPECT_EQ(*padded, std::size_t{1024});
}

TEST(DataCollectorDetailTest, ParseMemoryLimit_RejectsMaxSentinelAndInvalid) {
    using fq::benchmark::detail::parseMemoryLimit;
    EXPECT_FALSE(parseMemoryLimit("max").has_value());
    EXPECT_FALSE(parseMemoryLimit("").has_value());
    EXPECT_FALSE(parseMemoryLimit("abc").has_value());
    EXPECT_FALSE(parseMemoryLimit("0").has_value());
    // cgroup v1 无限额哨兵
    EXPECT_FALSE(parseMemoryLimit("9223372036854771712").has_value());
}

TEST(DataCollectorDetailTest, ParseCpuMax_ParsesQuotaAndCeil) {
    using fq::benchmark::detail::parseCpuMax;
    const auto exact = parseCpuMax("400000 100000");
    ASSERT_TRUE(exact.has_value());
    EXPECT_EQ(*exact, 4);

    const auto fractional = parseCpuMax("250000 100000");
    ASSERT_TRUE(fractional.has_value());
    EXPECT_EQ(*fractional, 3);

    EXPECT_FALSE(parseCpuMax("max 100000").has_value());
    EXPECT_FALSE(parseCpuMax("garbage").has_value());
}

TEST(DataCollectorDetailTest, ParseCpuQuota_ParsesV1AndRejectsUnlimited) {
    using fq::benchmark::detail::parseCpuQuota;
    const auto quota = parseCpuQuota("400000", "100000");
    ASSERT_TRUE(quota.has_value());
    EXPECT_EQ(*quota, 4);
    EXPECT_FALSE(parseCpuQuota("-1", "100000").has_value());
    EXPECT_FALSE(parseCpuQuota("0", "100000").has_value());
}

TEST(DataCollectorDetailTest, ParseCgroupV2Path_ParsesRootAndSubpath) {
    using fq::benchmark::detail::parseCgroupV2Path;
    const auto root = parseCgroupV2Path("0::/\n");
    ASSERT_TRUE(root.has_value());
    EXPECT_EQ(*root, "/");

    const auto subpath = parseCgroupV2Path("0::/user.slice/session.scope\n");
    ASSERT_TRUE(subpath.has_value());
    EXPECT_EQ(*subpath, "/user.slice/session.scope");
    EXPECT_FALSE(parseCgroupV2Path("1:name=systemd:/legacy\n").has_value());
}

TEST(DataCollectorDetailTest, ParseCgroupV1ControllerPath_ParsesMemoryAndCpu) {
    using fq::benchmark::detail::parseCgroupV1ControllerPath;
    const std::string content =
        "2:memory:/docker/abc\n"
        "3:cpu,cpuacct:/docker/abc\n";

    const auto memory = parseCgroupV1ControllerPath(content, "memory");
    ASSERT_TRUE(memory.has_value());
    EXPECT_EQ(*memory, "/docker/abc");

    const auto cpu = parseCgroupV1ControllerPath(content, "cpu");
    ASSERT_TRUE(cpu.has_value());
    EXPECT_EQ(*cpu, "/docker/abc");
    EXPECT_FALSE(parseCgroupV1ControllerPath(content, "cpuset").has_value());
}

TEST(DataCollectorDetailTest, JoinCgroupPath_HandlesRootAndSubpath) {
    namespace detail = fq::benchmark::detail;
    EXPECT_EQ(detail::joinCgroupPath("/sys/fs/cgroup", "/", "memory.max"),
              "/sys/fs/cgroup/memory.max");
    EXPECT_EQ(detail::joinCgroupPath("/sys/fs/cgroup", "/docker/abc", "memory.max"),
              "/sys/fs/cgroup/docker/abc/memory.max");
    EXPECT_EQ(detail::joinCgroupPath("/sys/fs/cgroup/cpu", "/x", "cpu.max"),
              "/sys/fs/cgroup/cpu/x/cpu.max");
}

// --- 环境相关：只验证可观察契约 ---

TEST(DataCollectorTest, CpuModel_IsNotEmpty) {
    const auto model = fq::benchmark::DataCollector::getCpuModel();
    EXPECT_FALSE(model.empty());

#if defined(__linux__)
    // 只有存在至少一个信息源时，才要求型号不是架构名/unknown；
    // distroless 等既无 lscpu 又无 device-tree/DMI 的环境允许回退到 uname -p。
    const auto readable = [](const char* path) {
        std::ifstream input(path);
        return input.good();
    };
    const bool hasSource = std::system("command -v lscpu >/dev/null 2>&1") == 0 ||
        readable("/proc/device-tree/model") || readable("/sys/firmware/devicetree/base/model") ||
        readable("/sys/devices/virtual/dmi/id/product_name");
    if (hasSource) {
        EXPECT_NE(model, "unknown");
#if defined(__aarch64__) || defined(__arm64__)
        EXPECT_NE(model, "aarch64");
        EXPECT_NE(model, "arm64");
#endif
    }
#endif
}

TEST(DataCollectorTest, CoreCount_RespectsCpuAffinityAndQuota) {
    const int cores = fq::benchmark::DataCollector::getCoreCount();
    EXPECT_GT(cores, 0);
    const unsigned hardwareCores = std::thread::hardware_concurrency();
    if (hardwareCores > 0) {
        EXPECT_LE(cores, static_cast<int>(hardwareCores));
    }
}

TEST(DataCollectorTest, MemoryBytes_IsPositiveOnLinux) {
#ifdef __linux__
    EXPECT_GT(fq::benchmark::DataCollector::getMemoryBytes(), 0U);
#else
    GTEST_SKIP() << "getMemoryBytes 目前只读取 Linux sysinfo/cgroup";
#endif
}

TEST(DataCollectorTest, CollectSystemMetadata_FillsCoreFields) {
    const auto metadata = fq::benchmark::DataCollector::collectSystemMetadata();
    EXPECT_FALSE(metadata.cpuModel.empty());
    EXPECT_GT(metadata.coreCount, 0);
#ifdef __linux__
    EXPECT_GT(metadata.memoryBytes, 0U);
#endif
    EXPECT_FALSE(metadata.compilerVersion.empty());
}

}  // namespace
