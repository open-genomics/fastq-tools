/**
 * @file interfaces.h
 * @brief 处理管道算子接口：ReadPredicateInterface（过滤）与 ReadMutatorInterface（修改）
 */

#pragma once

#include "fqtools/io/fastq_io.h"

namespace fq::processing {

/**
 * @brief 读段谓词接口
 * @details 判断读段是否满足过滤条件（true = 保留，false = 过滤）
 * @note 并行执行后端下，同一实例会被多个工作线程并发调用 evaluate()，
 *       且实例在整次运行中共享。实现必须线程安全：不持有未同步的可变状态
 *       （内置谓词的计数器已做分片处理）。
 */
class ReadPredicateInterface {
public:
    virtual ~ReadPredicateInterface() = default;

    [[nodiscard]] virtual auto evaluate(const fq::io::FastqRecord& read) const -> bool = 0;
};

/**
 * @brief 读段修改器接口
 * @details 对读段进行变换（修剪、截断等）。修改 FastqRecord 时需注意其指向 Batch 内存。
 * @note 并行执行后端下，同一实例会被多个工作线程并发调用 process()，
 *       但任一时刻每条记录只被一个线程处理。实现必须线程安全：
 *       不持有未同步的可变状态（内置修改器均为无状态）。
 */
class ReadMutatorInterface {
public:
    virtual ~ReadMutatorInterface() = default;

    /**
     * @brief 对单条读段应用变换
     * @return 记录是否被本调用改变（任一字段）。管道据此累计 modifiedReads，
     *         免去修改前后的全记录比较热点；未实际改动时必须返回 false。
     */
    virtual auto process(fq::io::FastqRecord& read) -> bool = 0;
};

}  // namespace fq::processing
