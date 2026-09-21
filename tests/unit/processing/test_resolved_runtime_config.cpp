/**
 * @file test_resolved_runtime_config.cpp
 * @brief 统一运行时配置的单元测试
 * @details 合并 test_runtime_policy.cpp 和 test_pipeline_execution_plan.cpp 的测试
 *
 */

#include "processing/resolved_runtime_config.h"
#include <fqtools/error/error.h>
#include <fqtools/processing/processing_options.h>
#include <gtest/gtest.h>

namespace fq::processing {

TEST(ResolvedRuntimeConfigTest, DerivesMemoryBoundedLiveTokenCountFromProcessingOptions) {
    ProcessingOptions options;
    options.threadCount = 4;
    options.memoryLimitBytes = 64ULL * 1024ULL * 1024ULL;

    const auto config = resolveRuntimeConfig(options);

    EXPECT_EQ(config.readChunkBytes, 1ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(config.batchCapacityBytes, 4ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(config.writerBufferBytes, 256ULL * 1024ULL);
    EXPECT_GE(config.maxLiveTokens, 1U);
    EXPECT_EQ(config.threadCount, 4U);
    EXPECT_EQ(config.executionMode, ExecutionMode::Parallel);
}

TEST(ResolvedRuntimeConfigTest, RejectsMemoryLimitBelowMinimumRunSet) {
    ProcessingOptions options;
    options.memoryLimitBytes = 1ULL * 1024ULL * 1024ULL;

    EXPECT_THROW(static_cast<void>(resolveRuntimeConfig(options)), fq::error::ConfigurationError);
}

TEST(ResolvedRuntimeConfigTest, FallsBackToSequentialModeWhenCustomIoIsPresent) {
    ProcessingOptions options;
    options.threadCount = 4;

    const auto config =
        resolveRuntimeConfig(options, /*hasCustomReader*/ true, /*hasCustomWriter*/ false);

    EXPECT_EQ(config.executionMode, ExecutionMode::Sequential);
    EXPECT_EQ(config.threadCount, 4U);
}

TEST(ResolvedRuntimeConfigTest, SelectsParallelModeWithDefaultOptions) {
    ProcessingOptions options;
    options.threadCount = 8;

    const auto config = resolveRuntimeConfig(options);

    EXPECT_EQ(config.executionMode, ExecutionMode::Parallel);
    EXPECT_EQ(config.threadCount, 8U);
    EXPECT_GE(config.maxLiveTokens, 4U);
}

TEST(ResolvedRuntimeConfigTest, AppliesLowMemoryProfile) {
    ProcessingOptions options;
    options.threadCount = 4;
    options.profile = ProcessingProfile::LowMemory;

    const auto config = resolveRuntimeConfig(options);

    EXPECT_EQ(config.readChunkBytes, 256ULL * 1024ULL);
    EXPECT_EQ(config.batchCapacityBytes, 1ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(config.writerBufferBytes, 64ULL * 1024ULL);
    EXPECT_EQ(config.maxLiveTokens, 2U);  // Fixed for LowMemory
}

TEST(ResolvedRuntimeConfigTest, AppliesHighThroughputProfile) {
    ProcessingOptions options;
    options.threadCount = 16;
    options.profile = ProcessingProfile::HighThroughput;

    const auto config = resolveRuntimeConfig(options);

    EXPECT_EQ(config.readChunkBytes, 4ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(config.batchCapacityBytes, 16ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(config.writerBufferBytes, 256ULL * 1024ULL);
    EXPECT_EQ(config.executionMode, ExecutionMode::Parallel);
    EXPECT_GE(config.maxLiveTokens, 4U);
}

TEST(ResolvedRuntimeConfigTest, UsesSequentialModeForSingleThread) {
    ProcessingOptions options;
    options.threadCount = 1;

    const auto config = resolveRuntimeConfig(options);

    EXPECT_EQ(config.executionMode, ExecutionMode::Sequential);
    EXPECT_EQ(config.threadCount, 1U);
}

TEST(ResolvedRuntimeConfigTest, ExplicitBatchCapacityOverridesProfileDefault) {
    ProcessingOptions options;
    options.threadCount = 4;
    options.batchCapacityBytes = 32ULL * 1024ULL * 1024ULL;  // 超长记录（ONT）场景

    const auto config = resolveRuntimeConfig(options);

    EXPECT_EQ(config.batchCapacityBytes, 32ULL * 1024ULL * 1024ULL);
    // 覆盖值参与每 token 内存核算
    EXPECT_GE(config.memoryPerTokenBytes, 32ULL * 1024ULL * 1024ULL);
}

TEST(ResolvedRuntimeConfigTest, RejectsZeroBatchCapacity) {
    ProcessingOptions options;
    options.batchCapacityBytes = 0;

    EXPECT_THROW(static_cast<void>(resolveRuntimeConfig(options)), std::invalid_argument);
}

}  // namespace fq::processing
