#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "fixture_loader.h"
#include "test_helpers.h"

#include "processing/execution_backend.h"
#include "processing/execution_runtime.h"
#include "processing/resolved_runtime_config.h"
#include <fqtools/error/error.h>
#include <fqtools/io/fastq_reader.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace fq::processing {
namespace {

struct ReaderCallState {
    size_t requestedMaxRecords = 0;
    size_t callCount = 0;
};

struct WriterCallState {
    size_t writeCalls = 0;
    size_t finishCalls = 0;
    bool failOnFinish = false;
    bool failOnWrite = false;
};

class RecordingWriter final : public fq::io::IWriter {
public:
    explicit RecordingWriter(std::shared_ptr<WriterCallState> state) : state_(std::move(state)) {}

    auto write(const fq::io::FastqBatch& batch) -> std::uint64_t override {
        ++state_->writeCalls;
        if (state_->failOnWrite) {
            throw fq::error::IOError("recording-writer", 5);
        }
        return batch.buffer().size();
    }

    void finish() override {
        ++state_->finishCalls;
        if (state_->failOnFinish) {
            throw fq::error::IOError("recording-writer", 5);
        }
    }

private:
    std::shared_ptr<WriterCallState> state_;
};

class RecordingLimitReader final : public fq::io::IReader {
public:
    explicit RecordingLimitReader(std::shared_ptr<ReaderCallState> state)
        : state_(std::move(state)) {}

    auto nextBatch(fq::io::FastqBatch& /*batch*/, size_t maxRecords) -> bool override {
        state_->requestedMaxRecords = maxRecords;
        ++state_->callCount;
        return false;
    }

private:
    std::shared_ptr<ReaderCallState> state_;
};

struct BatchSummary {
    std::vector<std::string> firstIds;
    std::vector<size_t> batchSizes;
    std::vector<std::uint64_t> committedBytes;
    size_t totalReads = 0;
};

class RecordingCommandAdapter {
public:
    using result_type = BatchSummary;

    // 置位后 processBatch 抛异常，用于验证并行管线中的异常传播
    bool failOnBatch = false;

    auto makeResult() const -> result_type {
        return {};
    }

    auto processBatch(fq::io::FastqBatch& batch) const -> result_type {
        if (failOnBatch) {
            throw std::runtime_error("adapter batch failure");
        }
        result_type partial;
        partial.batchSizes.push_back(batch.records().size());
        partial.totalReads = batch.records().size();
        if (!batch.records().empty()) {
            partial.firstIds.push_back(std::string(batch.records().front().id));
        }
        return partial;
    }

    void afterCommit(result_type& partial, std::uint64_t committedBytes) const {
        partial.committedBytes.push_back(committedBytes);
    }

    void merge(result_type& total, result_type partial) const {
        total.totalReads += partial.totalReads;
        total.firstIds.insert(
            total.firstIds.end(), partial.firstIds.begin(), partial.firstIds.end());
        total.batchSizes.insert(
            total.batchSizes.end(), partial.batchSizes.begin(), partial.batchSizes.end());
        total.committedBytes.insert(total.committedBytes.end(),
                                    partial.committedBytes.begin(),
                                    partial.committedBytes.end());
    }
};

auto writeFastqInput(const fq::test::TempDirectory& tempDir,
                     std::string_view filename,
                     std::string_view content) -> std::string {
    const auto path = tempDir.path() / filename;
    std::ofstream out(path);
    out << content;
    return path.string();
}

auto writeGeneratedFastqInput(const fq::test::TempDirectory& tempDir,
                              std::string_view filename,
                              size_t recordCount) -> std::string {
    const auto path = tempDir.path() / filename;
    std::ofstream out(path);
    out << fq::test::TestDataGenerator::generateFastQRecords(recordCount, 20);
    return path.string();
}

}  // namespace

TEST(ExecutionRuntimeTest, ExecuteProcessesBatchesThroughSingleAdapter) {
    fq::test::TempDirectory tempDir("execution_runtime_");
    ExecutionRuntime runtime;
    RecordingCommandAdapter adapter;

    ExecutionRuntimeRequest request;
    request.inputPath = writeFastqInput(tempDir,
                                        "input.fastq",
                                        "@read1\nACGT\n+\nIIII\n"
                                        "@read2\nTTTT\n+\n####\n");
    request.options.batchSize = 1;
    request.options.threadCount = 1;

    const auto outcome = runtime.execute(request, adapter);

    EXPECT_EQ(outcome.result.totalReads, 2U);
    EXPECT_EQ(outcome.metrics.batchCount, 2U);
    EXPECT_THAT(outcome.result.firstIds, ::testing::ElementsAre("read1", "read2"));
    EXPECT_THAT(outcome.result.batchSizes, ::testing::ElementsAre(1U, 1U));
}

TEST(ExecutionRuntimeTest, ExecutePassesConfiguredBatchSizeThroughReaderContract) {
    auto state = std::make_shared<ReaderCallState>();
    ExecutionRuntime runtime(std::make_unique<RecordingLimitReader>(state));
    RecordingCommandAdapter adapter;

    ExecutionRuntimeRequest request;
    request.options.batchSize = 37;
    request.options.threadCount = 1;

    const auto outcome = runtime.execute(request, adapter);

    EXPECT_EQ(outcome.metrics.batchCount, 0U);
    EXPECT_EQ(state->callCount, 1U);
    EXPECT_EQ(state->requestedMaxRecords, 37U);
}

TEST(ExecutionRuntimeTest, ExecuteRespectsConfiguredBatchSize) {
    fq::test::TempDirectory tempDir("execution_runtime_");
    ExecutionRuntime runtime;
    RecordingCommandAdapter adapter;

    ExecutionRuntimeRequest request;
    request.inputPath = writeGeneratedFastqInput(tempDir, "input.fastq", 250);
    request.options.batchSize = 100;
    request.options.threadCount = 1;

    const auto outcome = runtime.execute(request, adapter);

    EXPECT_EQ(outcome.result.totalReads, 250U);
    EXPECT_THAT(outcome.result.batchSizes, ::testing::ElementsAre(100U, 100U, 50U));
}

TEST(ExecutionRuntimeTest, ExecuteReportsCommittedBytesForCommittedBatches) {
    fq::test::TempDirectory tempDir("execution_runtime_");
    ExecutionRuntime runtime;
    RecordingCommandAdapter adapter;

    ExecutionRuntimeRequest request;
    request.inputPath = writeFastqInput(tempDir,
                                        "input.fastq",
                                        "@read1\nACGT\n+\nIIII\n"
                                        "@read2\nGGGG\n+\nIIII\n");
    request.outputPath = (tempDir.path() / "output.fastq").string();
    request.options.batchSize = 1;
    request.options.threadCount = 1;

    const auto outcome = runtime.execute(request, adapter);
    const auto committedTotal = std::accumulate(outcome.result.committedBytes.begin(),
                                                outcome.result.committedBytes.end(),
                                                std::uint64_t{0});

    ASSERT_EQ(outcome.result.committedBytes.size(), 2U);
    EXPECT_THAT(outcome.result.committedBytes, ::testing::Each(::testing::Gt(0U)));
    EXPECT_EQ(outcome.metrics.committedBytes, committedTotal);
    EXPECT_TRUE(std::filesystem::exists(*request.outputPath));
    EXPECT_EQ(outcome.metrics.committedBytes, std::filesystem::file_size(*request.outputPath));
}

TEST(ExecutionRuntimeTest, ExecuteFinishesInjectedWriterExactlyOnce) {
    fq::test::TempDirectory tempDir("execution_runtime_");
    auto state = std::make_shared<WriterCallState>();
    ExecutionRuntime runtime({}, std::make_shared<RecordingWriter>(state));
    RecordingCommandAdapter adapter;

    ExecutionRuntimeRequest request;
    request.inputPath = writeFastqInput(tempDir, "input.fastq", "@read1\nACGT\n+\nIIII\n");
    request.options.batchSize = 1;
    request.options.threadCount = 1;

    static_cast<void>(runtime.execute(request, adapter));

    EXPECT_EQ(state->writeCalls, 1U);
    EXPECT_EQ(state->finishCalls, 1U);
}

TEST(ExecutionRuntimeTest, PropagatesWriterFinishFailure) {
    fq::test::TempDirectory tempDir("execution_runtime_");
    auto state = std::make_shared<WriterCallState>();
    state->failOnFinish = true;
    ExecutionRuntime runtime({}, std::make_shared<RecordingWriter>(state));
    RecordingCommandAdapter adapter;

    ExecutionRuntimeRequest request;
    request.inputPath = writeFastqInput(tempDir, "input.fastq", "@read1\nACGT\n+\nIIII\n");
    request.options.batchSize = 1;
    request.options.threadCount = 1;

    EXPECT_THROW(static_cast<void>(runtime.execute(request, adapter)), fq::error::IOError);
    EXPECT_EQ(state->finishCalls, 1U);
}

TEST(ExecutionRuntimeTest, RejectsInputAndOutputAliasing) {
    fq::test::TempDirectory tempDir("execution_runtime_");
    ExecutionRuntime runtime;
    RecordingCommandAdapter adapter;

    ExecutionRuntimeRequest request;
    request.inputPath = writeFastqInput(tempDir, "input.fastq", "@read1\nACGT\n+\nIIII\n");
    request.outputPath = request.inputPath;
    request.options.batchSize = 1;
    request.options.threadCount = 1;

    EXPECT_THROW(static_cast<void>(runtime.execute(request, adapter)),
                 fq::error::ConfigurationError);
    std::ifstream in(request.inputPath);
    EXPECT_TRUE(in.good());
}

TEST(ExecutionRuntimeTest, ExecuteValidatesProcessingOptionsBeforeTraversal) {
    fq::test::TempDirectory tempDir("execution_runtime_");
    ExecutionRuntime runtime;
    RecordingCommandAdapter adapter;

    ExecutionRuntimeRequest request;
    request.inputPath = writeFastqInput(tempDir, "input.fastq", "@read1\nACGT\n+\nIIII\n");
    request.options.batchSize = 0;
    request.options.threadCount = 1;

    EXPECT_THROW(static_cast<void>(runtime.execute(request, adapter)), std::invalid_argument);
}

TEST(ExecutionRuntimeTest, ValidateRejectsUnreasonablyLargeBatchSize) {
    // 超大 batch-size 会在 FastqBatch 构造时按记录数预分配内存，
    // 必须在参数校验阶段拒绝而不是运行期 bad_alloc/OOM
    fq::processing::ProcessingOptions options;
    options.batchSize = 100'000'000'000ULL;
    options.threadCount = 1;
    EXPECT_THROW(static_cast<void>(options.validate()), std::invalid_argument);

    fq::processing::ProcessingOptions boundaryOk;
    boundaryOk.batchSize = 1'000'000U;
    boundaryOk.threadCount = 1;
    EXPECT_NO_THROW(static_cast<void>(boundaryOk.validate()));
}

TEST(ExecutionRuntimeTest, ExecuteKeepsBatchCommitOrderInParallelMode) {
    fq::test::TempDirectory tempDir("execution_runtime_");
    ExecutionRuntime runtime;
    RecordingCommandAdapter adapter;

    ExecutionRuntimeRequest request;
    request.inputPath = writeFastqInput(tempDir,
                                        "input.fastq",
                                        "@read1\nACGT\n+\nIIII\n"
                                        "@read2\nTTTT\n+\nIIII\n");
    request.outputPath = (tempDir.path() / "output.fastq").string();
    request.options.batchSize = 1;
    request.options.threadCount = 2;

    const auto outcome = runtime.execute(request, adapter);

    EXPECT_EQ(outcome.metrics.batchCount, 2U);
    EXPECT_THAT(outcome.result.firstIds, ::testing::ElementsAre("read1", "read2"));
    EXPECT_THAT(outcome.result.committedBytes, ::testing::Each(::testing::Gt(0U)));
}

TEST(ExecutionRuntimeTest, ExecuteBackendsHonorTheSameObservableContract) {
    fq::test::TempDirectory tempDir("execution_runtime_");
    const auto input = writeGeneratedFastqInput(tempDir, "input.fastq", 5);

    auto executeWith = [&input](ExecutionBackendPreference backend) {
        ExecutionRuntime runtime;
        RecordingCommandAdapter adapter;
        ExecutionRuntimeRequest request;
        request.inputPath = input;
        request.options.batchSize = 2;
        request.options.threadCount = 2;
        request.backend = backend;
        return runtime.execute(request, adapter);
    };

    const auto sequential = executeWith(ExecutionBackendPreference::Sequential);
    const auto oneTbb = executeWith(ExecutionBackendPreference::OneTbb);

    EXPECT_EQ(oneTbb.result.totalReads, sequential.result.totalReads);
    EXPECT_EQ(oneTbb.result.firstIds, sequential.result.firstIds);
    EXPECT_EQ(oneTbb.result.batchSizes, sequential.result.batchSizes);
    EXPECT_EQ(oneTbb.metrics.batchCount, sequential.metrics.batchCount);
    EXPECT_EQ(oneTbb.metrics.committedBytes, sequential.metrics.committedBytes);
}

// ---------------------------------------------------------------------------
// oneTBB 后端异常路径。ExecutionRuntime 对自定义 writer 强制回退串行后端
// （resolveRuntimeConfig 的契约），因此经 runtime 注入的 writer 永远到不了
// 并行后端；这里直连 createOneTbbExecutionBackend() 验证并行管线中
// adapter/writer 异常的传播与 finish 语义。
// ---------------------------------------------------------------------------
class AnyBatchSummaryOperation final : public ExecutionOperation {
public:
    explicit AnyBatchSummaryOperation(bool failOnProcess = false) : failOnProcess_(failOnProcess) {}

    auto makeResult() -> std::any override {
        return BatchSummary{};
    }

    auto processBatch(fq::io::FastqBatch& batch) -> std::any override {
        if (failOnProcess_) {
            throw std::runtime_error("operation batch failure");
        }
        BatchSummary partial;
        partial.totalReads = batch.records().size();
        return partial;
    }

    void afterCommit(std::any& /*partial*/, std::uint64_t /*committedBytes*/) override {}

    void merge(std::any& total, std::any partial) override {
        std::any_cast<BatchSummary&>(total).totalReads +=
            std::any_cast<BatchSummary&>(partial).totalReads;
    }

private:
    bool failOnProcess_;
};

auto makeOneTbbContext(const fq::test::TempDirectory& tempDir,
                       std::shared_ptr<fq::io::IWriter> writer) -> ExecutionBackendContext {
    ProcessingOptions options;
    options.batchSize = 1;
    options.threadCount = 2;

    ExecutionBackendContext context;
    context.reader = std::make_shared<fq::io::FastqReader>(
        writeFastqInput(tempDir, "backend_input.fastq", "@read1\nACGT\n+\nIIII\n"));
    context.writer = std::move(writer);
    context.config = resolveRuntimeConfig(options, false, false);
    context.batchSize = options.batchSize;
    return context;
}

TEST(ExecutionRuntimeTest, ExecutePropagatesAdapterFailureInParallelMode) {
    fq::test::TempDirectory tempDir("execution_runtime_");
    ExecutionRuntime runtime;
    RecordingCommandAdapter adapter;
    adapter.failOnBatch = true;

    ExecutionRuntimeRequest request;
    request.inputPath = writeFastqInput(tempDir, "input.fastq", "@read1\nACGT\n+\nIIII\n");
    request.outputPath = (tempDir.path() / "output.fastq").string();
    request.options.batchSize = 1;
    request.options.threadCount = 2;

    // 并行批次中抛出的异常必须传播出 execute 且不得挂起（TBB 管线异常传播回归）
    EXPECT_THROW(static_cast<void>(runtime.execute(request, adapter)), std::runtime_error);
}

TEST(OneTbbBackendTest, PropagatesAdapterFailureWithoutFinishingWriter) {
    fq::test::TempDirectory tempDir("execution_runtime_");
    auto state = std::make_shared<WriterCallState>();
    auto context = makeOneTbbContext(tempDir, std::make_shared<RecordingWriter>(state));

    AnyBatchSummaryOperation operation(/*failOnProcess=*/true);
    auto backend = createOneTbbExecutionBackend();

    EXPECT_THROW(static_cast<void>(backend->execute(std::move(context), operation)),
                 std::runtime_error);
    EXPECT_EQ(state->finishCalls, 0U);
}

TEST(OneTbbBackendTest, PropagatesWriterWriteFailure) {
    fq::test::TempDirectory tempDir("execution_runtime_");
    auto state = std::make_shared<WriterCallState>();
    state->failOnWrite = true;
    auto context = makeOneTbbContext(tempDir, std::make_shared<RecordingWriter>(state));

    AnyBatchSummaryOperation operation;
    auto backend = createOneTbbExecutionBackend();

    EXPECT_THROW(static_cast<void>(backend->execute(std::move(context), operation)),
                 fq::error::IOError);
    EXPECT_EQ(state->finishCalls, 0U);
}

TEST(OneTbbBackendTest, PropagatesWriterFinishFailure) {
    fq::test::TempDirectory tempDir("execution_runtime_");
    auto state = std::make_shared<WriterCallState>();
    state->failOnFinish = true;
    auto context = makeOneTbbContext(tempDir, std::make_shared<RecordingWriter>(state));

    AnyBatchSummaryOperation operation;
    auto backend = createOneTbbExecutionBackend();

    EXPECT_THROW(static_cast<void>(backend->execute(std::move(context), operation)),
                 fq::error::IOError);
    EXPECT_EQ(state->finishCalls, 1U);
}

TEST(OneTbbBackendTest, FinishesWriterExactlyOnceOnSuccess) {
    fq::test::TempDirectory tempDir("execution_runtime_");
    auto state = std::make_shared<WriterCallState>();
    auto context = makeOneTbbContext(tempDir, std::make_shared<RecordingWriter>(state));

    AnyBatchSummaryOperation operation;
    auto backend = createOneTbbExecutionBackend();

    auto outcome = backend->execute(std::move(context), operation);
    const auto summary = std::any_cast<BatchSummary>(std::move(outcome.result));

    EXPECT_EQ(summary.totalReads, 1U);
    EXPECT_EQ(state->writeCalls, 1U);
    EXPECT_EQ(state->finishCalls, 1U);
    EXPECT_EQ(outcome.metrics.batchCount, 1U);
}

}  // namespace fq::processing
