/**
 * @file concurrency_benchmark.cpp
 * @brief 真实生产流水线的线程数与批大小扫描
 * @details 与固定口径的生产基线分开，避免探索性矩阵改变既有快照或拖慢默认套件。
 */

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "benchmark_support.h"

#include <benchmark/benchmark.h>
#include <fqtools/processing/mutators/quality_trimmer.h>
#include <fqtools/processing/predicates.h>
#include <fqtools/processing/processing_pipeline_interface.h>
#include <fqtools/statistics/interfaces.h>

namespace fq::benchmark {

namespace {

auto makeProcessingOptions(std::size_t threadCount,
                           std::size_t batchSize,
                           fq::processing::ProcessingProfile profile =
                               fq::processing::ProcessingProfile::Default)
    -> fq::processing::ProcessingOptions {
    fq::processing::ProcessingOptions options;
    options.threadCount = threadCount;
    options.batchSize = batchSize;
    options.profile = profile;
    return options;
}

void configureCombinedFilter(fq::processing::Pipeline& pipeline) {
    pipeline.addReadMutator(std::make_unique<fq::processing::QualityTrimmer>(25.0, 100));
    pipeline.addReadPredicate(std::make_unique<fq::processing::MinLengthPredicate>(100));
    pipeline.addReadPredicate(std::make_unique<fq::processing::MinQualityPredicate>(25.0));
    pipeline.addReadPredicate(std::make_unique<fq::processing::MaxNRatioPredicate>(0.1));
}

auto makeOutputPath(std::string_view kind, std::size_t threadCount, std::size_t batchSize)
    -> std::filesystem::path {
    return std::filesystem::temp_directory_path() /
        ("fastqtools-concurrency-" + std::string(kind) + "-" + std::to_string(threadCount) + "-" +
         std::to_string(batchSize) + ".fastq");
}

void setConcurrencyCounters(::benchmark::State& state,
                            std::size_t threadCount,
                            std::size_t batchSize) {
    state.counters["threads"] = static_cast<double>(threadCount);
    state.counters["batch_records"] = static_cast<double>(batchSize);
}

void benchmarkFilter(::benchmark::State& state,
                     bool writeOutput,
                     fq::processing::ProcessingProfile profile,
                     bool configureFilter) {
    const auto inputPath = BenchmarkDataset::path();
    const auto inputBytes = BenchmarkDataset::fileSize();
    const auto threadCount = static_cast<std::size_t>(state.range(0));
    const auto batchSize = static_cast<std::size_t>(state.range(1));
    const auto outputPath =
        makeOutputPath(writeOutput ? "filter-plain" : "filter-cpu", threadCount, batchSize);

    std::uint64_t passedReads = 0;
    std::uint64_t filteredReads = 0;
    std::uint64_t modifiedReads = 0;
    for (auto _ : state) {
        fq::processing::Pipeline pipeline;
        pipeline.setInputPath(inputPath.string());
        if (writeOutput) {
            pipeline.setOutputPath(outputPath.string());
        }
        pipeline.setProcessingOptions(makeProcessingOptions(threadCount, batchSize, profile));
        if (configureFilter) {
            configureCombinedFilter(pipeline);
        }

        const auto stats = pipeline.run();
        passedReads = stats.passedReads;
        filteredReads = stats.filteredReads;
        modifiedReads = stats.modifiedReads;
        ::benchmark::DoNotOptimize(passedReads);
    }

    if (writeOutput) {
        removeBenchmarkOutput(outputPath);
    }

    setThroughputCounters(state, kBenchmarkReadCount, inputBytes);
    setConcurrencyCounters(state, threadCount, batchSize);
    state.counters["passed_reads"] = static_cast<double>(passedReads);
    state.counters["filtered_reads"] = static_cast<double>(filteredReads);
    state.counters["modified_reads"] = static_cast<double>(modifiedReads);
}

void benchmarkReadQc(::benchmark::State& state) {
    const auto inputPath = BenchmarkDataset::path();
    const auto inputBytes = BenchmarkDataset::fileSize();
    const auto threadCount = static_cast<std::size_t>(state.range(0));
    const auto batchSize = static_cast<std::size_t>(state.range(1));

    for (auto _ : state) {
        fq::statistics::StatisticOptions options;
        options.inputFastqPath = inputPath.string();
        options.processing = makeProcessingOptions(threadCount, batchSize);
        fq::statistics::Calculator calculator(std::move(options));
        calculator.run();
    }

    setThroughputCounters(state, kBenchmarkReadCount, inputBytes);
    setConcurrencyCounters(state, threadCount, batchSize);
}

}  // namespace

BENCHMARK_CAPTURE(benchmarkFilter,
                  cpu,
                  false,
                  fq::processing::ProcessingProfile::Default,
                  true)
    ->ArgsProduct({{1, 2, 4, 8}, {1'000, 10'000, 50'000}})
    ->Unit(::benchmark::kMillisecond);

BENCHMARK_CAPTURE(benchmarkFilter,
                  cpu_no_filter,
                  false,
                  fq::processing::ProcessingProfile::Default,
                  false)
    ->ArgsProduct({{1, 2, 4, 8}, {1'000, 10'000, 50'000}})
    ->Unit(::benchmark::kMillisecond);

BENCHMARK_CAPTURE(benchmarkFilter,
                  plain_output,
                  true,
                  fq::processing::ProcessingProfile::Default,
                  true)
    ->ArgsProduct({{1, 2, 4, 8}, {1'000, 10'000, 50'000}})
    ->Unit(::benchmark::kMillisecond);

BENCHMARK_CAPTURE(benchmarkFilter,
                  plain_output_high_throughput,
                  true,
                  fq::processing::ProcessingProfile::HighThroughput,
                  true)
    ->ArgsProduct({{1, 2, 4, 8}, {10'000, 50'000}})
    ->Unit(::benchmark::kMillisecond);

// 名称不含 "stat"：runner 对该探索性矩阵使用 real_time，适合比较并行墙钟吞吐。
BENCHMARK(benchmarkReadQc)
    ->ArgsProduct({{1, 2, 4, 8}, {1'000, 10'000, 50'000}})
    ->Unit(::benchmark::kMillisecond);

}  // namespace fq::benchmark
