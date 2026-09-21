#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>
#include <fqtools/io/fastq_reader.h>
#include <fqtools/io/fastq_writer.h>

#include "benchmark/data_collector.h"

#ifdef __linux__
#include <unistd.h>
#endif

namespace fq::benchmark {

/// 向 Google Benchmark 注册系统元数据，使 JSON 输出自文档化
inline void registerSystemContext() {
    auto metadata = DataCollector::collectSystemMetadata();
    ::benchmark::AddCustomContext("git_commit", metadata.gitCommit);
    ::benchmark::AddCustomContext("git_branch", metadata.gitBranch);
    ::benchmark::AddCustomContext("cpu_model", metadata.cpuModel);
    ::benchmark::AddCustomContext("core_count", std::to_string(metadata.coreCount));
    ::benchmark::AddCustomContext("memory_bytes", std::to_string(metadata.memoryBytes));
    ::benchmark::AddCustomContext("compiler", metadata.compilerVersion);
    ::benchmark::AddCustomContext("os", metadata.osVersion);
    ::benchmark::AddCustomContext("timestamp", metadata.timestamp);
}

inline constexpr std::size_t kBenchmarkReadCount = 1'000'000;
inline constexpr std::size_t kBenchmarkReadLength = 150;
inline constexpr std::size_t kBenchmarkBatchSize = 10'000;
inline constexpr std::uint32_t kBenchmarkSeed = 42;

/**
 * @brief 固定种子的主基准数据集。
 * @details 数据只在进程内第一次访问时生成，所有 benchmark 共享同一输入。
 */
class BenchmarkDataset final {
public:
    [[nodiscard]] static auto path() -> const std::filesystem::path& {
        static const auto datasetPath = makePath();
        static const bool generated = generate(datasetPath);
        (void)generated;
        return datasetPath;
    }

    [[nodiscard]] static auto fileSize() -> std::uint64_t {
        return std::filesystem::file_size(path());
    }

private:
    [[nodiscard]] static auto makePath() -> std::filesystem::path {
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        std::string suffix = "benchmark-" + std::to_string(timestamp);
#ifdef __linux__
        suffix += "-" + std::to_string(static_cast<long long>(::getpid()));
#endif
        return std::filesystem::temp_directory_path() /
            ("fastqtools-" + suffix + "-1m-150-seed42.fastq");
    }

    static auto generate(const std::filesystem::path& outputPath) -> bool {
        if (std::filesystem::exists(outputPath) && std::filesystem::file_size(outputPath) > 0) {
            return true;
        }

        std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("failed to create benchmark dataset: " + outputPath.string());
        }

        std::mt19937 generator(kBenchmarkSeed);
        std::uniform_int_distribution<int> baseDistribution(0, 3);
        std::uniform_int_distribution<int> qualityDistribution(20, 40);
        constexpr std::string_view bases = "ATGC";
        std::string sequence(kBenchmarkReadLength, 'A');
        std::string quality(kBenchmarkReadLength, 'I');

        for (std::size_t readIndex = 0; readIndex < kBenchmarkReadCount; ++readIndex) {
            for (std::size_t position = 0; position < kBenchmarkReadLength; ++position) {
                sequence[position] = bases[static_cast<std::size_t>(baseDistribution(generator))];
                quality[position] = static_cast<char>(33 + qualityDistribution(generator));
            }

            output << "@read_" << readIndex << '\n' << sequence << "\n+\n" << quality << '\n';
        }

        if (!output) {
            throw std::runtime_error("failed to write benchmark dataset: " + outputPath.string());
        }
        return true;
    }
};

[[nodiscard]] inline auto loadBenchmarkBatches() -> const std::vector<fq::io::FastqBatch>& {
    static const auto batches = [] {
        std::vector<fq::io::FastqBatch> loaded;
        loaded.reserve(kBenchmarkReadCount / kBenchmarkBatchSize);

        fq::io::FastqReader reader(BenchmarkDataset::path());
        fq::io::FastqBatch batch(4 * 1024 * 1024, kBenchmarkBatchSize);
        while (reader.nextBatch(batch, kBenchmarkBatchSize)) {
            loaded.push_back(std::move(batch));
            batch = fq::io::FastqBatch(4 * 1024 * 1024, kBenchmarkBatchSize);
        }
        return loaded;
    }();
    return batches;
}

[[nodiscard]] inline auto peakResidentBytes() -> std::uint64_t {
#ifdef __linux__
    std::ifstream statm("/proc/self/statm");
    std::uint64_t totalPages = 0;
    std::uint64_t residentPages = 0;
    if (statm >> totalPages >> residentPages) {
        const auto pageSize = static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE));
        return residentPages * pageSize;
    }
#endif
    return 0;
}

inline void setThroughputCounters(::benchmark::State& state,
                                  std::uint64_t readCount,
                                  std::uint64_t inputBytes) {
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * readCount));
    state.SetBytesProcessed(static_cast<std::int64_t>(state.iterations() * inputBytes));
    state.counters["reads_per_s"] =
        ::benchmark::Counter(static_cast<double>(readCount),
                             ::benchmark::Counter::kIsIterationInvariantRate);
    state.counters["mib_per_s"] = ::benchmark::Counter(
        static_cast<double>(inputBytes) / (1024.0 * 1024.0),
        ::benchmark::Counter::kIsIterationInvariantRate);
    state.counters["peak_memory_bytes"] = static_cast<double>(peakResidentBytes());
}

inline void removeBenchmarkOutput(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
}

struct WriterBenchmarkSpec {
    std::string_view name;
    int compressionLevel = 6;
    fq::io::FastqWriterCompressionMode compression = fq::io::FastqWriterCompressionMode::Gzip;
};

[[nodiscard]] inline auto writerOutputPath(const WriterBenchmarkSpec& spec, std::string_view api)
    -> std::filesystem::path {
    const auto suffix =
        spec.compression == fq::io::FastqWriterCompressionMode::None ? ".fastq" : ".fastq.gz";
    return std::filesystem::temp_directory_path() /
        ("fastqtools-writer-" + BenchmarkDataset::path().stem().string() + "-" +
         std::string(spec.name) + "-" + std::string(api) + suffix);
}

}  // namespace fq::benchmark
