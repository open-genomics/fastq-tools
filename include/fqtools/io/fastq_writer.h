/**
 * @file fastq_writer.h
 * @brief FASTQ 文件写入器，支持 gzip 压缩和原子发布
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "fastq_io.h"
#include "interfaces.h"

namespace fq::io {

enum class FastqWriterCompressionMode : std::uint8_t {
    Auto,  ///< 根据文件扩展名自动检测
    Gzip,
    None,
};

struct FastqWriterOptions {
    size_t zlibBufferBytes = static_cast<size_t>(128) * 1024;
    size_t outputBufferBytes = static_cast<size_t>(256) * 1024;
    int compressionLevel = 6;  ///< 1-9
    FastqWriterCompressionMode compression = FastqWriterCompressionMode::Auto;
};

/**
 * @brief FASTQ 文件写入器（move-only）
 * @details 普通文件先写同目录临时文件，finish() 成功后原子 rename 发布。
 * 标准输出和特殊文件目标直接写入；未调用 finish() 时普通文件的临时内容不会发布。
 */
class FastqWriter : public IWriter {
public:
    explicit FastqWriter(const std::string& path);
    FastqWriter(const std::string& path, const FastqWriterOptions& options);
    ~FastqWriter() override;

    FastqWriter(const FastqWriter&) = delete;
    FastqWriter& operator=(const FastqWriter&) = delete;
    FastqWriter(FastqWriter&&) noexcept;
    FastqWriter& operator=(FastqWriter&&) noexcept;

    auto write(const FastqBatch& batch) -> std::uint64_t override;
    void finish() override;

    /// 写入单条记录（不在 IWriter 接口中）
    void write(const FastqRecord& record);

    [[nodiscard]] auto isOpen() const -> bool;
    [[nodiscard]] auto totalUncompressedBytes() const -> std::uint64_t;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fq::io
