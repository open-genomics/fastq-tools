/**
 * @file resolved_runtime_config.cpp
 * @brief 统一运行时配置的实现
 * @details 合并 runtime_policy.cpp 和 pipeline_execution_plan.cpp 的逻辑
 *
 */

#include "processing/resolved_runtime_config.h"

#include "fqtools/error/error.h"
#include "fqtools/io/fastq_io.h"

#include <algorithm>
#include <limits>

namespace fq::processing {

namespace {

constexpr size_t kDefaultReadChunkBytes = 1 * 1024 * 1024;
constexpr size_t kDefaultBatchCapacityBytes = 4 * 1024 * 1024;
constexpr size_t kDefaultWriterBufferBytes = 256 * 1024;
constexpr size_t kZlibBufferBytes = 128 * 1024;

auto applyProfileDefaults(ResolvedRuntimeConfig& config, ProcessingProfile profile) -> void {
    switch (profile) {
        case ProcessingProfile::Default:
            config.readChunkBytes = kDefaultReadChunkBytes;
            config.batchCapacityBytes = kDefaultBatchCapacityBytes;
            config.writerBufferBytes = kDefaultWriterBufferBytes;
            break;
        case ProcessingProfile::LowMemory:
            config.readChunkBytes = 256 * 1024;
            config.batchCapacityBytes = 1 * 1024 * 1024;
            config.writerBufferBytes = 64 * 1024;
            break;
        case ProcessingProfile::HighThroughput:
            config.readChunkBytes = 4 * 1024 * 1024;
            config.batchCapacityBytes = 16 * 1024 * 1024;
            config.writerBufferBytes = 256 * 1024;
            break;
    }
}

auto resolveMaxLiveTokens(size_t memoryLimitBytes,
                          size_t memoryPerTokenBytes,
                          size_t threadCount,
                          ProcessingProfile profile) -> size_t {
    const size_t requested = profile == ProcessingProfile::LowMemory
        ? 2
        : std::max(static_cast<size_t>(4), threadCount * 2);

    if (memoryLimitBytes == 0) {
        return requested;
    }
    if (memoryPerTokenBytes == 0 || memoryLimitBytes < memoryPerTokenBytes) {
        throw fq::error::ConfigurationError(
            "memory limit is below the minimum runtime working set");
    }

    return std::max(static_cast<size_t>(1),
                    std::min(requested, memoryLimitBytes / memoryPerTokenBytes));
}

auto estimateMemoryPerToken(const ResolvedRuntimeConfig& config, size_t batchSize) -> size_t {
    constexpr size_t kRecordBytes = sizeof(fq::io::FastqRecord);
    if (batchSize > std::numeric_limits<size_t>::max() / kRecordBytes) {
        throw fq::error::ConfigurationError("batch size is too large for memory accounting");
    }

    const size_t recordBytes = batchSize * kRecordBytes;
    const size_t components[] = {config.batchCapacityBytes,
                                 recordBytes,
                                 config.readChunkBytes,
                                 config.writerBufferBytes,
                                 config.zlibBufferBytes};
    size_t total = 0;
    for (const size_t component : components) {
        if (component > std::numeric_limits<size_t>::max() - total) {
            throw fq::error::ConfigurationError("runtime memory accounting overflow");
        }
        total += component;
    }
    return total;
}

}  // namespace

auto resolveRuntimeConfig(const ProcessingOptions& options,
                          bool hasCustomReader,
                          bool hasCustomWriter) -> ResolvedRuntimeConfig {
    options.validate();

    ResolvedRuntimeConfig config;
    config.zlibBufferBytes = kZlibBufferBytes;
    config.threadCount = std::max(static_cast<size_t>(1), options.threadCount);
    config.allocationTelemetryEnabled = false;

    // Apply profile defaults
    applyProfileDefaults(config, options.profile);
    // 显式指定的批缓冲上限覆盖预设默认值（超长记录场景），
    // 并参与下方每 token 内存核算，受 memoryLimitBytes 总量约束
    if (options.batchCapacityBytes.has_value()) {
        config.batchCapacityBytes = options.batchCapacityBytes.value();
    }
    config.batchSize = options.batchSize;
    config.memoryPerTokenBytes = estimateMemoryPerToken(config, options.batchSize);

    // Resolve max live tokens
    config.maxLiveTokens = resolveMaxLiveTokens(options.memoryLimitBytes.value_or(0),
                                                config.memoryPerTokenBytes,
                                                config.threadCount,
                                                options.profile);

    // Determine execution mode
    // Parallel mode requires: no custom I/O, threadCount > 1。
    // 自定义 reader/writer 当前强制走串行后端：这是保守契约而非技术限制——
    // TBB 后端中两者均只在串行阶段被访问（serial filter / serial_in_order 提交），
    // 并行是安全的；但自定义实现可能对执行模式有隐含假设，
    // 在没有真实调用方验证前不放开（公共行为变更需有测试与用例支撑）
    if (!hasCustomReader && !hasCustomWriter && config.threadCount > 1) {
        config.executionMode = ExecutionMode::Parallel;
    } else {
        config.executionMode = ExecutionMode::Sequential;
    }

    return config;
}

}  // namespace fq::processing
