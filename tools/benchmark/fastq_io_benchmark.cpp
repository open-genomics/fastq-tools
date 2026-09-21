#include <cstddef>
#include <cstdint>

#include "benchmark_support.h"

#include <benchmark/benchmark.h>

namespace fq::benchmark {

namespace {

void benchmarkReader(::benchmark::State& state) {
    const auto inputPath = BenchmarkDataset::path();
    const auto inputBytes = BenchmarkDataset::fileSize();

    for (auto _ : state) {
        fq::io::FastqReader reader(inputPath.string());
        fq::io::FastqBatch batch(4 * 1024 * 1024, kBenchmarkBatchSize);
        std::size_t totalReads = 0;
        while (reader.nextBatch(batch, kBenchmarkBatchSize)) {
            totalReads += batch.size();
        }
        ::benchmark::DoNotOptimize(totalReads);
    }

    setThroughputCounters(state, kBenchmarkReadCount, inputBytes);
}

void benchmarkWriter(::benchmark::State& state, const WriterBenchmarkSpec& spec, bool batchApi) {
    const auto& batches = loadBenchmarkBatches();
    const auto inputBytes = BenchmarkDataset::fileSize();
    const auto outputPath = writerOutputPath(spec, batchApi ? "batch" : "single");

    for (auto _ : state) {
        fq::io::FastqWriterOptions options;
        options.compression = spec.compression;
        options.compressionLevel = spec.compressionLevel;
        fq::io::FastqWriter writer(outputPath.string(), options);
        std::uint64_t writtenBytes = 0;

        if (batchApi) {
            for (const auto& batch : batches) {
                writtenBytes += writer.write(batch);
            }
        } else {
            for (const auto& batch : batches) {
                for (const auto& record : batch) {
                    writer.write(record);
                }
            }
            writtenBytes = writer.totalUncompressedBytes();
        }

        ::benchmark::DoNotOptimize(writtenBytes);
    }

    removeBenchmarkOutput(outputPath);
    setThroughputCounters(state, kBenchmarkReadCount, inputBytes);
}

}  // namespace

BENCHMARK(benchmarkReader)->Unit(::benchmark::kMillisecond);

BENCHMARK_CAPTURE(benchmarkWriter,
                  plain_single,
                  WriterBenchmarkSpec{"plain", 6, fq::io::FastqWriterCompressionMode::None},
                  false)
    ->Unit(::benchmark::kMillisecond);
BENCHMARK_CAPTURE(benchmarkWriter,
                  plain_batch,
                  WriterBenchmarkSpec{"plain", 6, fq::io::FastqWriterCompressionMode::None},
                  true)
    ->Unit(::benchmark::kMillisecond);

BENCHMARK_CAPTURE(benchmarkWriter,
                  gzip_1_single,
                  WriterBenchmarkSpec{"gzip-1", 1, fq::io::FastqWriterCompressionMode::Gzip},
                  false)
    ->Unit(::benchmark::kMillisecond);
BENCHMARK_CAPTURE(benchmarkWriter,
                  gzip_1_batch,
                  WriterBenchmarkSpec{"gzip-1", 1, fq::io::FastqWriterCompressionMode::Gzip},
                  true)
    ->Unit(::benchmark::kMillisecond);

BENCHMARK_CAPTURE(benchmarkWriter,
                  gzip_6_single,
                  WriterBenchmarkSpec{"gzip-6", 6, fq::io::FastqWriterCompressionMode::Gzip},
                  false)
    ->Unit(::benchmark::kMillisecond);
BENCHMARK_CAPTURE(benchmarkWriter,
                  gzip_6_batch,
                  WriterBenchmarkSpec{"gzip-6", 6, fq::io::FastqWriterCompressionMode::Gzip},
                  true)
    ->Unit(::benchmark::kMillisecond);

BENCHMARK_CAPTURE(benchmarkWriter,
                  gzip_9_single,
                  WriterBenchmarkSpec{"gzip-9", 9, fq::io::FastqWriterCompressionMode::Gzip},
                  false)
    ->Unit(::benchmark::kMillisecond);
BENCHMARK_CAPTURE(benchmarkWriter,
                  gzip_9_batch,
                  WriterBenchmarkSpec{"gzip-9", 9, fq::io::FastqWriterCompressionMode::Gzip},
                  true)
    ->Unit(::benchmark::kMillisecond);

}  // namespace fq::benchmark
