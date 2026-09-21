/**
 * @file resolved_runtime_config.h
 * @brief 统一的运行时配置结构
 * @details 合并 RuntimePolicy 和 PipelineExecutionPlan，提供完整的运行时配置。
 *          消除多层配置转换，提高局部性。
 *
 */

#pragma once

#include "fqtools/processing/processing_options.h"

#include <cstddef>
#include <cstdint>

namespace fq::processing {

/**
 * @brief 执行模式枚举
 */
enum class ExecutionMode : std::uint8_t {
    Sequential,  ///< 串行执行
    Parallel,    ///< 并行执行（TBB）
};

/**
 * @brief 统一的运行时配置结构
 * @details 包含 I/O 配置和执行配置，由 ProcessingOptions 派生。
 *          替代原有的 RuntimePolicy 和 PipelineExecutionPlan。
 */
struct ResolvedRuntimeConfig {
    /// @name I/O 配置
    /// @{
    size_t readChunkBytes = 1 * 1024 * 1024;      ///< 读取块大小（字节）
    size_t zlibBufferBytes = 128 * 1024;          ///< zlib 内部缓冲区大小
    size_t batchCapacityBytes = 4 * 1024 * 1024;  ///< 批次缓冲区容量（字节）
    size_t batchSize = 10000;                     ///< 批次记录数（用于容量和预算）
    size_t writerBufferBytes = 256 * 1024;        ///< 写入器缓冲区大小
    size_t memoryPerTokenBytes = 0;               ///< 每个并行 token 的保守内存预算
    /// @}

    /// @name 执行配置
    /// @{
    size_t threadCount = 1;                                   ///< 线程数量
    size_t maxLiveTokens = 4;                                 ///< 最大并行批次（TBB tokens）
    ExecutionMode executionMode = ExecutionMode::Sequential;  ///< 执行模式
    bool allocationTelemetryEnabled = false;                  ///< 是否启用内存遥测
    /// @}
};

/**
 * @brief 从用户选项解析运行时配置
 * @param options 用户可见的处理选项
 * @param hasCustomReader 是否使用自定义 Reader
 * @param hasCustomWriter 是否使用自定义 Writer
 * @return 解析后的运行时配置
 */
[[nodiscard]] auto resolveRuntimeConfig(const ProcessingOptions& options,
                                        bool hasCustomReader = false,
                                        bool hasCustomWriter = false) -> ResolvedRuntimeConfig;

}  // namespace fq::processing
