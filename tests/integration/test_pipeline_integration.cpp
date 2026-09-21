#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "fixture_loader.h"
#include "test_helpers.h"

#include <fqtools/fq.h>
#include <gtest/gtest.h>

namespace fq::test {

namespace {

struct TestRecordStorage {
    std::deque<std::string> ids;
    std::deque<std::string> seqs;
    std::deque<std::string> quals;
    std::deque<std::string> pluses;

    auto makeBatch(std::string id, std::string seq) -> fq::io::FastqBatch {
        ids.push_back(std::move(id));
        seqs.push_back(std::move(seq));
        quals.push_back("IIII");
        pluses.push_back("+");

        const size_t idx = ids.size() - 1;
        fq::io::FastqBatch batch(1024, 1);
        batch.records().push_back(
            fq::io::FastqRecord{ids[idx], {}, seqs[idx], quals[idx], pluses[idx]});
        return batch;
    }
};

class VectorReader final : public fq::io::IReader {
public:
    explicit VectorReader(std::vector<fq::io::FastqBatch> batches) : batches_(std::move(batches)) {}

    auto nextBatch(fq::io::FastqBatch& batch, size_t /*maxRecords*/) -> bool override {
        if (cursor_ >= batches_.size()) {
            return false;
        }
        batch = std::move(batches_[cursor_++]);
        return true;
    }

private:
    std::vector<fq::io::FastqBatch> batches_;
    size_t cursor_ = 0;
};

}  // namespace

class PipelineIntegrationTest : public FastQToolsTest {};

TEST_F(PipelineIntegrationTest, ReaderPipelineWriterPreservesCustomPlusLine) {
    const auto input = tempDir_.path() / "input.fastq";
    const auto output = tempDir_.path() / "output.fastq";

    {
        std::ofstream out(input);
        out << "@read1 comment\n"
            << "ACGT\n"
            << "+read1 comment\n"
            << "IIII\n"
            << "@read2\n"
            << "TTTT\n"
            << "+\n"
            << "####\n";
    }

    fq::processing::Pipeline pipeline;
    pipeline.setInputPath(input.string());
    pipeline.setOutputPath(output.string());

    fq::processing::ProcessingOptions options;
    options.threadCount = 1;
    options.batchSize = 2;
    pipeline.setProcessingOptions(options);

    const auto stats = pipeline.run();

    EXPECT_EQ(stats.totalReads, 2);
    EXPECT_EQ(stats.passedReads, 2);

    const auto content = FixtureLoader::loadTextFile(output);
    EXPECT_NE(content.find("@read1 comment\nACGT\n+read1 comment\nIIII\n"), std::string::npos);
    EXPECT_NE(content.find("@read2\nTTTT\n+\n####\n"), std::string::npos);
}

TEST_F(PipelineIntegrationTest, StatisticCalculatorWritesConfiguredPhredHeader) {
    const auto input = tempDir_.path() / "input.fastq";
    const auto output = tempDir_.path() / "stats.txt";

    {
        std::ofstream out(input);
        out << "@read1\n"
            << "A\n"
            << "+\n"
            << "@\n";
    }

    fq::statistics::StatisticOptions options;
    options.inputFastqPath = input.string();
    options.outputStatPath = output.string();
    options.processing.batchSize = 1;
    options.processing.threadCount = 1;
    options.qualityEncoding = 64;

    fq::statistics::Calculator calculator(options);
    calculator.run();

    const auto content = FixtureLoader::loadTextFile(output);
    EXPECT_NE(content.find("#PhredQual\t64\n"), std::string::npos);
    EXPECT_NE(content.find("#ReadNum\t1\n"), std::string::npos);
}

TEST_F(PipelineIntegrationTest, PipelineRunsWithMultipleThreads) {
    const auto input = tempDir_.path() / "input.fastq";
    const auto output = tempDir_.path() / "output.fastq";

    {
        std::ofstream out(input);
        out << "@read1\n"
            << "ACGT\n"
            << "+\n"
            << "IIII\n"
            << "@read2\n"
            << "TTTT\n"
            << "+\n"
            << "IIII\n";
    }

    fq::processing::Pipeline pipeline;
    pipeline.setInputPath(input.string());
    pipeline.setOutputPath(output.string());

    fq::processing::ProcessingOptions options;
    options.threadCount = 2;
    options.batchSize = 1;
    pipeline.setProcessingOptions(options);

    const auto stats = pipeline.run();

    EXPECT_EQ(stats.totalReads, 2);
    EXPECT_EQ(stats.passedReads, 2);
    EXPECT_TRUE(std::filesystem::exists(output));
}

TEST_F(PipelineIntegrationTest, PipelineReportsModifiedReadsAcrossRuntimeBatches) {
    const auto input = tempDir_.path() / "input.fastq";
    const auto output = tempDir_.path() / "output.fastq";

    {
        std::ofstream out(input);
        out << "@read1\n"
            << "ACGT\n"
            << "+\n"
            << "!!II\n"
            << "@read2\n"
            << "TTTT\n"
            << "+\n"
            << "IIII\n";
    }

    fq::processing::Pipeline pipeline;
    pipeline.setInputPath(input.string());
    pipeline.setOutputPath(output.string());

    fq::processing::ProcessingOptions options;
    options.threadCount = 2;
    options.batchSize = 1;
    pipeline.setProcessingOptions(options);
    pipeline.addReadMutator(std::make_unique<fq::processing::QualityTrimmer>(20.0));

    const auto stats = pipeline.run();

    EXPECT_EQ(stats.totalReads, 2);
    EXPECT_EQ(stats.modifiedReads, 1);
    EXPECT_NE(FixtureLoader::loadTextFile(output).find("@read1\nGT\n+\nII\n"), std::string::npos);
}

TEST_F(PipelineIntegrationTest, AppliesTrimmingBeforeMinimumLengthPredicate) {
    const auto input = tempDir_.path() / "input.fastq";
    const auto output = tempDir_.path() / "output.fastq";

    {
        std::ofstream out(input);
        out << "@read1\n"
            << "ACGT\n"
            << "+\n"
            << "!!II\n";
    }

    fq::processing::Pipeline pipeline;
    pipeline.setInputPath(input.string());
    pipeline.setOutputPath(output.string());
    fq::processing::ProcessingOptions options;
    options.threadCount = 1;
    options.batchSize = 1;
    pipeline.setProcessingOptions(options);
    pipeline.addReadMutator(std::make_unique<fq::processing::QualityTrimmer>(20.0));
    pipeline.addReadPredicate(std::make_unique<fq::processing::MinLengthPredicate>(3));

    const auto stats = pipeline.run();

    EXPECT_EQ(stats.totalReads, 1);
    EXPECT_EQ(stats.passedReads, 0);
    EXPECT_EQ(stats.filteredReads, 1);
    EXPECT_EQ(stats.modifiedReads, 0);
    EXPECT_TRUE(FixtureLoader::loadTextFile(output).empty());
}

TEST_F(PipelineIntegrationTest, AppliesTrimmingBeforeAverageQualityPredicate) {
    const auto input = tempDir_.path() / "input.fastq";
    const auto output = tempDir_.path() / "output.fastq";

    {
        std::ofstream out(input);
        out << "@read1\n"
            << "ACGT\n"
            << "+\n"
            << "!!II\n";
    }

    fq::processing::Pipeline pipeline;
    pipeline.setInputPath(input.string());
    pipeline.setOutputPath(output.string());
    fq::processing::ProcessingOptions options;
    options.threadCount = 1;
    options.batchSize = 1;
    pipeline.setProcessingOptions(options);
    pipeline.addReadMutator(std::make_unique<fq::processing::QualityTrimmer>(20.0));
    pipeline.addReadPredicate(std::make_unique<fq::processing::MinQualityPredicate>(30.0));

    const auto stats = pipeline.run();

    EXPECT_EQ(stats.passedReads, 1);
    EXPECT_EQ(stats.filteredReads, 0);
    EXPECT_EQ(stats.modifiedReads, 1);
    EXPECT_NE(FixtureLoader::loadTextFile(output).find("@read1\nGT\n+\nII\n"), std::string::npos);
}

TEST_F(PipelineIntegrationTest, AppliesTrimmingBeforeNRatioPredicate) {
    const auto input = tempDir_.path() / "input.fastq";
    const auto output = tempDir_.path() / "output.fastq";

    {
        std::ofstream out(input);
        out << "@read1\n"
            << "ACGTNN\n"
            << "+\n"
            << "IIIIII\n";
    }

    fq::processing::Pipeline pipeline;
    pipeline.setInputPath(input.string());
    pipeline.setOutputPath(output.string());
    fq::processing::ProcessingOptions options;
    options.threadCount = 1;
    options.batchSize = 1;
    pipeline.setProcessingOptions(options);
    pipeline.addReadMutator(
        std::make_unique<fq::processing::AdapterTrimmer>(std::vector<std::string>{"NN"}, 2, 0));
    pipeline.addReadPredicate(std::make_unique<fq::processing::MaxNRatioPredicate>(0.0));

    const auto stats = pipeline.run();

    EXPECT_EQ(stats.passedReads, 1);
    EXPECT_EQ(stats.filteredReads, 0);
    EXPECT_EQ(stats.modifiedReads, 1);
    EXPECT_NE(FixtureLoader::loadTextFile(output).find("@read1\nACGT\n+\nIIII\n"),
              std::string::npos);
}

TEST_F(PipelineIntegrationTest, StatisticCalculatorRunsWithMultipleThreads) {
    const auto input = tempDir_.path() / "input.fastq";
    const auto output = tempDir_.path() / "stats.txt";

    {
        std::ofstream out(input);
        out << "@read1\n"
            << "A\n"
            << "+\n"
            << "I\n"
            << "@read2\n"
            << "T\n"
            << "+\n"
            << "I\n";
    }

    fq::statistics::StatisticOptions options;
    options.inputFastqPath = input.string();
    options.outputStatPath = output.string();
    options.processing.batchSize = 1;
    options.processing.threadCount = 2;

    fq::statistics::Calculator calculator(options);
    calculator.run();

    const auto content = FixtureLoader::loadTextFile(output);
    EXPECT_NE(content.find("#ReadNum\t2\n"), std::string::npos);
}

TEST_F(PipelineIntegrationTest, StatisticCalculatorAggregatesAcrossRuntimeBatches) {
    const auto input = tempDir_.path() / "input.fastq";
    const auto output = tempDir_.path() / "stats.txt";

    {
        std::ofstream out(input);
        out << "@read1\n"
            << "A\n"
            << "+\n"
            << "I\n"
            << "@read2\n"
            << "T\n"
            << "+\n"
            << "I\n"
            << "@read3\n"
            << "G\n"
            << "+\n"
            << "I\n";
    }

    fq::statistics::StatisticOptions options;
    options.inputFastqPath = input.string();
    options.outputStatPath = output.string();
    options.processing.batchSize = 1;
    options.processing.threadCount = 2;

    fq::statistics::Calculator calculator(options);
    calculator.run();

    const auto content = FixtureLoader::loadTextFile(output);
    EXPECT_NE(content.find("#ReadNum\t3\n"), std::string::npos);
}

TEST_F(PipelineIntegrationTest, PipelineRunsInLowMemoryMode) {
    const auto input = tempDir_.path() / "input.fastq";
    const auto output = tempDir_.path() / "output.fastq";

    {
        std::ofstream out(input);
        out << "@read1\n"
            << "ACGT\n"
            << "+\n"
            << "IIII\n";
    }

    fq::processing::Pipeline pipeline;
    pipeline.setInputPath(input.string());
    pipeline.setOutputPath(output.string());

    fq::processing::ProcessingOptions options;
    options.threadCount = 2;
    options.batchSize = 1;
    options.profile = fq::processing::ProcessingProfile::LowMemory;
    pipeline.setProcessingOptions(options);

    const auto stats = pipeline.run();

    EXPECT_EQ(stats.totalReads, 1);
    EXPECT_EQ(stats.passedReads, 1);
}

TEST_F(PipelineIntegrationTest, PipelineRequiresResettingCustomReaderBeforeRerun) {
    TestRecordStorage storage;
    fq::processing::Pipeline pipeline;

    fq::processing::ProcessingOptions options;
    options.threadCount = 1;
    options.batchSize = 1;
    pipeline.setProcessingOptions(options);
    pipeline.setReader(std::make_unique<VectorReader>(
        std::vector<fq::io::FastqBatch>{storage.makeBatch("read1", "ACGT")}));

    const auto firstRunStats = pipeline.run();
    EXPECT_EQ(firstRunStats.totalReads, 1);

    EXPECT_THROW(static_cast<void>(pipeline.run()), std::invalid_argument);
}

TEST_F(PipelineIntegrationTest, PipelineWithoutOperationsStillFiltersEmptyCustomRecords) {
    fq::io::FastqBatch batch(1024, 2);
    batch.records().push_back(fq::io::FastqRecord{"read1", {}, "ACGT", "IIII", "+"});
    batch.records().push_back(fq::io::FastqRecord{"empty", {}, {}, {}, "+"});

    fq::processing::Pipeline pipeline;
    fq::processing::ProcessingOptions options;
    options.threadCount = 1;
    options.batchSize = 2;
    pipeline.setProcessingOptions(options);
    pipeline.setReader(
        std::make_unique<VectorReader>(std::vector<fq::io::FastqBatch>{std::move(batch)}));

    const auto stats = pipeline.run();

    EXPECT_EQ(stats.totalReads, 2);
    EXPECT_EQ(stats.passedReads, 1);
    EXPECT_EQ(stats.filteredReads, 1);
}

TEST_F(PipelineIntegrationTest, StatisticCalculatorRunsInHighThroughputMode) {
    const auto input = tempDir_.path() / "input.fastq";
    const auto output = tempDir_.path() / "stats.txt";

    {
        std::ofstream out(input);
        out << "@read1\n"
            << "A\n"
            << "+\n"
            << "I\n";
    }

    fq::statistics::StatisticOptions options;
    options.inputFastqPath = input.string();
    options.outputStatPath = output.string();
    options.processing.batchSize = 1;
    options.processing.threadCount = 2;
    options.processing.profile = fq::processing::ProcessingProfile::HighThroughput;

    fq::statistics::Calculator calculator(options);
    calculator.run();

    const auto content = FixtureLoader::loadTextFile(output);
    EXPECT_NE(content.find("#ReadNum\t1\n"), std::string::npos);
}

TEST_F(PipelineIntegrationTest, StatisticCalculatorWritesSignatureSidecarWhenEnabled) {
    const auto input = tempDir_.path() / "input.fastq";
    const auto output = tempDir_.path() / "stats.txt";
    const auto sidecar = tempDir_.path() / "signatures.tsv";

    {
        std::ofstream out(input);
        out << "@read1\n"
            << "ACGTAAAA\n"
            << "+\n"
            << "IIIIIIII\n"
            << "@read2\n"
            << "ACGTAAAA\n"
            << "+\n"
            << "IIIIIIII\n"
            << "@read3\n"
            << "TTTTCCCC\n"
            << "+\n"
            << "IIIIIIII\n";
    }

    fq::statistics::StatisticOptions options;
    options.inputFastqPath = input.string();
    options.outputStatPath = output.string();
    options.signatureReportPath = sidecar.string();
    options.signatureKmerSize = 4;
    options.maxReportedSignatures = 10;
    options.duplicateEstimateSampleModulo = 1;
    options.processing.batchSize = 2;
    options.processing.threadCount = 2;

    fq::statistics::Calculator calculator(options);
    calculator.run();

    const auto report = FixtureLoader::loadTextFile(output);
    const auto signatures = FixtureLoader::loadTextFile(sidecar);
    EXPECT_NE(report.find("#DuplicateEstimate\t1\n"), std::string::npos);
    EXPECT_NE(signatures.find("summary\tduplicate_estimate\t1\n"), std::string::npos);
    EXPECT_NE(signatures.find("head_kmer\tACGT\t2\n"), std::string::npos);
}

// 回归：并行流水线必须在有效规模上保证输出保序。
// 历史保序测试仅 2 条记录，任何乱序 bug 在该规模下几乎不可能显现；
// 本测试以 10k 条唯一 ID 记录、4 线程、小批次运行，读回输出逐条比对。
// 若 serial_in_order 被误改为 parallel，此测试必然失败。
TEST_F(PipelineIntegrationTest, ParallelPipelinePreservesRecordOrderAtScale) {
    constexpr size_t kRecordCount = 10000;
    constexpr size_t kSeqLen = 50;

    const auto input = tempDir_.path() / "input.fastq";
    const auto output = tempDir_.path() / "output.fastq";

    // 确定性伪随机序列（LCG）：每条记录唯一且跨运行可复现
    std::vector<std::string> seqs(kRecordCount);
    std::vector<std::string> quals(kRecordCount);
    {
        std::ofstream out(input);
        ASSERT_TRUE(out.is_open());
        std::uint64_t state = 0x9E3779B97F4A7C15ULL;
        auto nextRandom = [&state]() -> std::uint64_t {
            state = state * 6364136223846793005ULL + 1442695040888963407ULL;
            return state >> 33;
        };
        constexpr std::string_view kBases = "ACGT";
        for (size_t i = 0; i < kRecordCount; ++i) {
            std::string seq(kSeqLen, 'A');
            std::string qual(kSeqLen, 'I');
            for (size_t j = 0; j < kSeqLen; ++j) {
                seq[j] = kBases[nextRandom() % 4];
                qual[j] = static_cast<char>(33 + nextRandom() % 41);  // q0..q40
            }
            seqs[i] = seq;
            quals[i] = qual;
            out << "@read_" << i << '\n' << seq << "\n+\n" << qual << '\n';
        }
    }

    fq::processing::Pipeline pipeline;
    pipeline.setInputPath(input.string());
    pipeline.setOutputPath(output.string());

    fq::processing::ProcessingOptions options;
    options.threadCount = 4;
    options.batchSize = 100;  // 小批次 → 100 个并行处理单元，最大化乱序机会
    pipeline.setProcessingOptions(options);

    const auto stats = pipeline.run();
    EXPECT_EQ(stats.totalReads, kRecordCount);
    EXPECT_EQ(stats.passedReads, kRecordCount);

    // 读回输出，逐条比对顺序与内容
    fq::io::FastqReader reader(output.string());
    fq::io::FastqBatch batch;
    size_t verified = 0;
    while (reader.nextBatch(batch)) {
        for (const auto& rec : batch) {
            ASSERT_LT(verified, kRecordCount) << "输出记录数超过输入";
            EXPECT_EQ(rec.id, "read_" + std::to_string(verified)) << "记录乱序，位置 " << verified;
            ASSERT_EQ(rec.seq.size(), kSeqLen);
            EXPECT_EQ(rec.seq, seqs[verified]);
            EXPECT_EQ(rec.qual, quals[verified]);
            ++verified;
        }
    }
    EXPECT_EQ(verified, kRecordCount);
}

}  // namespace fq::test
