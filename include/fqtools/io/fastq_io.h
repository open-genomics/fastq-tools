/**
 * @file fastq_io.h
 * @brief FASTQ I/O 核心数据结构：零拷贝记录视图与批量容器
 */

#pragma once

#include <string_view>
#include <vector>

namespace fq::io {

/**
 * @brief FASTQ 记录视图（零拷贝）
 * @details 所有字段均为 string_view，指向所属 FastqBatch 的连续内存。
 *          生命周期严格绑定到所属批次，不可逃逸。
 */
struct FastqRecord {
    std::string_view id;       ///< 序列 ID（不含 @ 前缀）
    std::string_view comment;  ///< ID 行的注释部分（可选）
    std::string_view seq;      ///< 序列内容（碱基）
    std::string_view qual;     ///< 质量值字符串
    std::string_view plus;     ///< '+' 行原文（含前导 +）

    [[nodiscard]] auto empty() const -> bool {
        return seq.empty();
    }

    [[nodiscard]] auto length() const -> size_t {
        return seq.size();
    }

    /// 校验序列与质量值长度是否匹配
    [[nodiscard]] auto validateLengths() const -> bool {
        return !seq.empty() && seq.size() == qual.size();
    }
};

/**
 * @brief FASTQ 批次数据容器
 * @details 拥有连续内存，记录视图指向其内部偏移。
 *          clear() 复用内存不释放，适用于循环处理场景。
 */
class FastqBatch {
public:
    explicit FastqBatch(size_t capacityBytes = 4 * 1024 * 1024) {
        buffer_.reserve(capacityBytes);
        records_.reserve(capacityBytes / 150);
    }

    FastqBatch(size_t capacityBytes, size_t expectedRecords) {
        buffer_.reserve(capacityBytes);
        records_.reserve(expectedRecords);
    }

    void clear() {
        buffer_.clear();
        records_.clear();
    }

    /// @name 迭代器支持
    /// @{
    [[nodiscard]] auto begin() const {
        return records_.begin();
    }
    [[nodiscard]] auto end() const {
        return records_.end();
    }
    [[nodiscard]] auto begin() {
        return records_.begin();
    }
    [[nodiscard]] auto end() {
        return records_.end();
    }
    /// @}

    [[nodiscard]] auto size() const -> size_t {
        return records_.size();
    }

    [[nodiscard]] auto empty() const -> bool {
        return records_.empty();
    }

    /// @name 原始数据访问（供 Reader/Writer 使用）
    /// @{
    [[nodiscard]] auto buffer() -> std::vector<char>& {
        return buffer_;
    }
    [[nodiscard]] auto records() -> std::vector<FastqRecord>& {
        return records_;
    }
    [[nodiscard]] auto buffer() const -> const std::vector<char>& {
        return buffer_;
    }
    /// @}

private:
    std::vector<char> buffer_;
    std::vector<FastqRecord> records_;
};

}  // namespace fq::io
