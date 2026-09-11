#pragma once

#include "fqtools/processing/interfaces.h"

#include <atomic>
#include <cstddef>
#include <string>

namespace fq::processing {

namespace detail {

/// 分片计数器：消除并行流水线上多工作线程对同一原子量的 cache line 竞争。
/// 线程首次自增时轮转领取固定分片（thread_local 缓存，热路径零额外开销），
/// 读取时汇总全部分片。
class ShardedCounter {
public:
    void increment() noexcept {
        shards_[shardIndex()].value.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] auto load() const noexcept -> std::size_t {
        std::size_t sum = 0;
        for (const auto& shard : shards_) {
            sum += shard.value.load(std::memory_order_relaxed);
        }
        return sum;
    }

private:
    static constexpr std::size_t kShardCount = 32;

    struct alignas(64) Shard {
        std::atomic<std::size_t> value{0};
    };

    static auto shardIndex() noexcept -> std::size_t {
        // thread_local const 具静态存储期，按 StaticConstantCase 命名（k + CamelCase），
        // 与同文件的 kShardCount 一致。
        thread_local const std::size_t kIndex = [] {
            static std::atomic<std::size_t> next{0};
            return next.fetch_add(1, std::memory_order_relaxed) % kShardCount;
        }();
        return kIndex;
    }

    Shard shards_[kShardCount];
};

}  // namespace detail

class MinQualityPredicate : public ReadPredicateInterface {
public:
    explicit MinQualityPredicate(double minQuality, int qualityEncoding = 33);
    auto evaluate(const fq::io::FastqRecord& read) const -> bool override;

    [[nodiscard]] auto getName() const -> std::string;
    [[nodiscard]] auto getDescription() const -> std::string;
    [[nodiscard]] auto getStatistics() const -> std::string;

private:
    double minQuality_;
    int qualityEncoding_;
    mutable detail::ShardedCounter totalEvaluated_;
    mutable detail::ShardedCounter passedCount_;

    // Helper to calculate average quality from string_view
    auto calculateAverageQuality(std::string_view qualityString) const -> double;
};

class MinLengthPredicate : public ReadPredicateInterface {
public:
    explicit MinLengthPredicate(size_t minLength);
    auto evaluate(const fq::io::FastqRecord& read) const -> bool override;

    [[nodiscard]] auto getName() const -> std::string;
    [[nodiscard]] auto getDescription() const -> std::string;
    [[nodiscard]] auto getStatistics() const -> std::string;

private:
    size_t minLength_;
    mutable detail::ShardedCounter totalEvaluated_;
    mutable detail::ShardedCounter passedCount_;
};

class MaxLengthPredicate : public ReadPredicateInterface {
public:
    explicit MaxLengthPredicate(size_t maxLength);
    auto evaluate(const fq::io::FastqRecord& read) const -> bool override;

    [[nodiscard]] auto getName() const -> std::string;
    [[nodiscard]] auto getDescription() const -> std::string;
    [[nodiscard]] auto getStatistics() const -> std::string;

private:
    size_t maxLength_;
    mutable detail::ShardedCounter totalEvaluated_;
    mutable detail::ShardedCounter passedCount_;
};

class MaxNRatioPredicate : public ReadPredicateInterface {
public:
    explicit MaxNRatioPredicate(double maxNRatio);
    auto evaluate(const fq::io::FastqRecord& read) const -> bool override;

    [[nodiscard]] auto getName() const -> std::string;
    [[nodiscard]] auto getDescription() const -> std::string;
    [[nodiscard]] auto getStatistics() const -> std::string;

private:
    double maxNRatio_;
    mutable detail::ShardedCounter totalEvaluated_;
    mutable detail::ShardedCounter passedCount_;

    auto calculateNRatio(std::string_view sequence) const -> double;
};

}  // namespace fq::processing
