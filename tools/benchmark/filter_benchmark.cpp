#include <cstddef>
#include <filesystem>
#include <memory>

#include "benchmark_support.h"

#include <benchmark/benchmark.h>
#include <fqtools/processing/mutators/quality_trimmer.h>
#include <fqtools/processing/predicates/min_quality_predicate.h>
#include <fqtools/processing/processing_pipeline_interface.h>

namespace fq::benchmark {

namespace {

enum class FilterScenario { Baseline, MinLength, MinQuality, MaxNRatio, Combined };

template <typename PipelineLike>
void configurePipeline(PipelineLike& pipeline, FilterScenario scenario) {
    switch (scenario) {
        case FilterScenario::Baseline:
            return;
        case FilterScenario::MinLength:
            pipeline.addReadPredicate(std::make_unique<fq::processing::MinLengthPredicate>(100));
            return;
        case FilterScenario::MinQuality:
            pipeline.addReadPredicate(std::make_unique<fq::processing::MinQualityPredicate>(25.0));
            return;
        case FilterScenario::MaxNRatio:
            pipeline.addReadPredicate(std::make_unique<fq::processing::MaxNRatioPredicate>(0.1));
            return;
        case FilterScenario::Combined:
            pipeline.addReadMutator(std::make_unique<fq::processing::QualityTrimmer>(25.0, 100));
            pipeline.addReadPredicate(std::make_unique<fq::processing::MinLengthPredicate>(100));
            pipeline.addReadPredicate(std::make_unique<fq::processing::MinQualityPredicate>(25.0));
            pipeline.addReadPredicate(std::make_unique<fq::processing::MaxNRatioPredicate>(0.1));
            return;
    }
}

void benchmarkFilter(::benchmark::State& state, FilterScenario scenario) {
    const auto inputPath = BenchmarkDataset::path();
    const auto inputBytes = BenchmarkDataset::fileSize();
    const auto outputPath = std::filesystem::temp_directory_path() /
        ("fastqtools-filter-" + std::to_string(static_cast<int>(scenario)) + ".fastq");

    std::uint64_t passedReads = 0;
    std::uint64_t filteredReads = 0;
    std::uint64_t modifiedReads = 0;
    for (auto _ : state) {
        fq::processing::Pipeline pipeline;
        pipeline.setInputPath(inputPath.string());
        pipeline.setOutputPath(outputPath.string());

        fq::processing::ProcessingOptions options;
        options.batchSize = kBenchmarkBatchSize;
        options.threadCount = 1;
        pipeline.setProcessingOptions(options);
        configurePipeline(pipeline, scenario);

        const auto stats = pipeline.run();
        passedReads = stats.passedReads;
        filteredReads = stats.filteredReads;
        modifiedReads = stats.modifiedReads;
        ::benchmark::DoNotOptimize(passedReads);
    }

    removeBenchmarkOutput(outputPath);
    setThroughputCounters(state, kBenchmarkReadCount, inputBytes);
    state.counters["passed_reads"] = static_cast<double>(passedReads);
    state.counters["filtered_reads"] = static_cast<double>(filteredReads);
    state.counters["modified_reads"] = static_cast<double>(modifiedReads);
}

}  // namespace

BENCHMARK_CAPTURE(benchmarkFilter, baseline, FilterScenario::Baseline)
    ->Unit(::benchmark::kMillisecond);
BENCHMARK_CAPTURE(benchmarkFilter, min_length, FilterScenario::MinLength)
    ->Unit(::benchmark::kMillisecond);
BENCHMARK_CAPTURE(benchmarkFilter, min_quality, FilterScenario::MinQuality)
    ->Unit(::benchmark::kMillisecond);
BENCHMARK_CAPTURE(benchmarkFilter, max_n_ratio, FilterScenario::MaxNRatio)
    ->Unit(::benchmark::kMillisecond);
BENCHMARK_CAPTURE(benchmarkFilter, combined_with_trim, FilterScenario::Combined)
    ->Unit(::benchmark::kMillisecond);

}  // namespace fq::benchmark
