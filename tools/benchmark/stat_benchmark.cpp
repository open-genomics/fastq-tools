#include <cstdint>

#include "benchmark_support.h"

#include "statistics/fq_statistic_worker.h"
#include <benchmark/benchmark.h>

namespace fq::benchmark {

namespace {

void benchmarkProductionStatisticWorker(::benchmark::State& state) {
    const auto& batches = loadBenchmarkBatches();
    const auto inputBytes = BenchmarkDataset::fileSize();

    fq::statistics::FqStatisticResult result;
    for (auto _ : state) {
        fq::statistics::FqStatisticWorker worker(33, 15, 1024);
        result = {};
        for (const auto& batch : batches) {
            result += worker.calculateStats(batch);
        }
        ::benchmark::DoNotOptimize(result.readCount);
        ::benchmark::DoNotOptimize(result.totalBases);
    }

    setThroughputCounters(state, kBenchmarkReadCount, inputBytes);
    state.counters["read_count"] = static_cast<double>(result.readCount);
    state.counters["total_bases"] = static_cast<double>(result.totalBases);
    state.counters["max_read_length"] = static_cast<double>(result.maxReadLength);
}

}  // namespace

BENCHMARK(benchmarkProductionStatisticWorker)->Unit(::benchmark::kMillisecond);

}  // namespace fq::benchmark
