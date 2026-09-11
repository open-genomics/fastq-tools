#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include "fixture_loader.h"

#include "statistics/fq_statistic.h"
#include <fqtools/processing/predicates.h>
#include <fqtools/processing/processing_pipeline_interface.h>
#include <gtest/gtest.h>

namespace {

auto writeFastqInput(const fq::test::TempDirectory& tempDir,
                     std::string_view filename,
                     std::string_view content) -> std::string {
    const auto path = tempDir.path() / filename;
    std::ofstream out(path);
    out << content;
    return path.string();
}

}  // namespace

TEST(PipelineReadStatisticsTest, CountsOnlyReadsThatPassPredicates) {
    fq::test::TempDirectory tempDir("pipeline_read_statistics_");
    const auto input = writeFastqInput(tempDir,
                                       "input.fastq",
                                       "@keep1\nACGT\n+\nIIII\n"
                                       "@drop\nAC\n+\nII\n"
                                       "@keep2\nTTTT\n+\nIIII\n");
    const auto output = (tempDir.path() / "output.fastq").string();

    fq::processing::Pipeline pipeline;
    pipeline.setInputPath(input);
    pipeline.setOutputPath(output);
    pipeline.addReadPredicate(std::make_unique<fq::processing::MinLengthPredicate>(4));
    pipeline.enableReadStatistics();

    fq::processing::ProcessingOptions options;
    options.threadCount = 2;
    options.batchSize = 1;
    pipeline.setProcessingOptions(options);

    const auto stats = pipeline.run();

    EXPECT_EQ(stats.totalReads, 3U);
    EXPECT_EQ(stats.passedReads, 2U);
    ASSERT_TRUE(pipeline.hasReadStatistics());

    const auto& qc = pipeline.readStatistics();
    EXPECT_EQ(qc.readCount, 2U);
    EXPECT_EQ(qc.totalBases, 8U);
    EXPECT_EQ(qc.maxReadLength, 4U);
}

TEST(PipelineReadStatisticsTest, ReadStatisticsThrowsWhenCollectionDisabled) {
    fq::test::TempDirectory tempDir("pipeline_read_statistics_disabled_");
    const auto input = writeFastqInput(tempDir, "input.fastq", "@read1\nACGT\n+\nIIII\n");
    const auto output = (tempDir.path() / "output.fastq").string();

    fq::processing::Pipeline pipeline;
    pipeline.setInputPath(input);
    pipeline.setOutputPath(output);

    fq::processing::ProcessingOptions options;
    options.threadCount = 1;
    options.batchSize = 1;
    pipeline.setProcessingOptions(options);

    static_cast<void>(pipeline.run());

    EXPECT_FALSE(pipeline.hasReadStatistics());
    EXPECT_THROW(static_cast<void>(pipeline.readStatistics()), std::logic_error);
}
