#include "fqtools/io/fastq_io.h"
#include "fqtools/processing/mutators.h"
#include "fqtools/processing/predicates.h"
#include "fqtools/processing/processing_pipeline_interface.h"
#include "fqtools/statistics/interfaces.h"

#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

TEST(PipelineSmokeTest, PipelineIsMoveOnlyConcreteApi) {
    static_assert(!std::is_copy_constructible_v<fq::processing::Pipeline>);
    static_assert(std::is_move_constructible_v<fq::processing::Pipeline>);
    fq::processing::Pipeline pipeline;
    auto moved = std::move(pipeline);
    static_cast<void>(moved);
    SUCCEED();
}

TEST(PipelineSmokeTest, ProcessingOptionsDefaults) {
    fq::processing::ProcessingOptions options;

    EXPECT_EQ(options.batchSize, 10000);
    EXPECT_EQ(options.threadCount, 1);
    EXPECT_EQ(options.profile, fq::processing::ProcessingProfile::Default);
    EXPECT_FALSE(options.memoryLimitBytes.has_value());
}

TEST(PipelineSmokeTest, StatisticOptionsDefaults) {
    fq::statistics::StatisticOptions options;

    EXPECT_EQ(options.processing.batchSize, 10000);
    EXPECT_EQ(options.processing.threadCount, 1);
    EXPECT_EQ(options.processing.profile, fq::processing::ProcessingProfile::Default);
}

TEST(PipelineSmokeTest, ProcessingProfileEnumExists) {
    // 验证三种预设存在
    auto defaultProfile = fq::processing::ProcessingProfile::Default;
    auto lowMemory = fq::processing::ProcessingProfile::LowMemory;
    auto highThroughput = fq::processing::ProcessingProfile::HighThroughput;

    EXPECT_NE(defaultProfile, lowMemory);
    EXPECT_NE(defaultProfile, highThroughput);
    EXPECT_NE(lowMemory, highThroughput);
}

TEST(PipelineSmokeTest, QualityTrimmerTrimsLowQualityEnds) {
    fq::processing::QualityTrimmer trimmer(20.0);

    fq::io::FastqRecord read{"read1", {}, "ACGT", "!!II", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "GT");
    EXPECT_EQ(read.qual, "II");
}

TEST(PipelineSmokeTest, QualityTrimmerSupportsThreePrimeMode) {
    fq::processing::QualityTrimmer trimmer(
        20.0, 1, fq::processing::QualityTrimmer::TrimMode::ThreePrime);

    fq::io::FastqRecord read{"read1", {}, "ACGT", "II!!", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "AC");
    EXPECT_EQ(read.qual, "II");
}

TEST(PipelineSmokeTest, QualityTrimmerSupportsFivePrimeMode) {
    fq::processing::QualityTrimmer trimmer(
        20.0, 1, fq::processing::QualityTrimmer::TrimMode::FivePrime);

    fq::io::FastqRecord read{"read1", {}, "ACGT", "!!II", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "GT");
    EXPECT_EQ(read.qual, "II");
}

TEST(PipelineSmokeTest, QualityTrimmerDropsReadShorterThanMinimumLength) {
    fq::processing::QualityTrimmer trimmer(20.0, 3);

    fq::io::FastqRecord read{"read1", {}, "ACGT", "!!II", "+"};
    trimmer.process(read);

    EXPECT_TRUE(read.seq.empty());
    EXPECT_TRUE(read.qual.empty());
}

TEST(PipelineSmokeTest, QualityTrimmerRespectsPhred64Encoding) {
    fq::processing::QualityTrimmer trimmer(
        20.0, 1, fq::processing::QualityTrimmer::TrimMode::Both, 64);

    fq::io::FastqRecord read{"read1", {}, "ACGT", "@@^^", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "GT");
    EXPECT_EQ(read.qual, "^^");
}

TEST(PipelineSmokeTest, MinQualityPredicateUsesConfiguredEncoding) {
    fq::processing::MinQualityPredicate predicate(30.0, 64);

    fq::io::FastqRecord read{"read1", {}, "ACGT", "^^^^", "+"};
    EXPECT_TRUE(predicate.evaluate(read));
}

TEST(PipelineSmokeTest, MinQualityPredicateRejectsEmptyQualityString) {
    fq::processing::MinQualityPredicate predicate(10.0);

    fq::io::FastqRecord read{"read1", {}, "ACGT", {}, "+"};
    EXPECT_FALSE(predicate.evaluate(read));
}

TEST(PipelineSmokeTest, LengthPredicatesApplyInclusiveBounds) {
    fq::io::FastqRecord read{"read1", {}, "ACGT", "IIII", "+"};

    EXPECT_TRUE(fq::processing::MinLengthPredicate(4).evaluate(read));
    EXPECT_TRUE(fq::processing::MaxLengthPredicate(4).evaluate(read));
    EXPECT_FALSE(fq::processing::MinLengthPredicate(5).evaluate(read));
    EXPECT_FALSE(fq::processing::MaxLengthPredicate(3).evaluate(read));
}

TEST(PipelineSmokeTest, MaxNRatioPredicateTreatsBothCasesOfNAsAmbiguous) {
    fq::processing::MaxNRatioPredicate predicate(0.25);

    fq::io::FastqRecord read{"read1", {}, "Annn", "IIII", "+"};
    EXPECT_FALSE(predicate.evaluate(read));
}

TEST(PipelineSmokeTest, AdapterTrimmerRemovesMatchedSuffix) {
    fq::processing::AdapterTrimmer trimmer({"TTAA"}, 3, 0);

    fq::io::FastqRecord read{"read1", {}, "ACGTTTAA", "IIIIIIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACGT");
    EXPECT_EQ(read.qual, "IIII");
}

TEST(PipelineSmokeTest, AdapterTrimmerLeavesReadUntouchedWhenNoAdapterFound) {
    fq::processing::AdapterTrimmer trimmer({"TTAA"}, 3, 0);

    fq::io::FastqRecord read{"read1", {}, "ACGTACGT", "IIIIIIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACGTACGT");
    EXPECT_EQ(read.qual, "IIIIIIII");
}

TEST(PipelineSmokeTest, PolyGTailTrimmerRemovesPolyGTail) {
    fq::processing::PolyTailTrimmer trimmer(fq::processing::PolyTailTrimmer::TailKind::PolyG, 4);

    fq::io::FastqRecord read{"read1", {}, "ACGTGGGG", "IIIIIIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACGT");
    EXPECT_EQ(read.qual, "IIII");
}

TEST(PipelineSmokeTest, PolyXTailTrimmerRemovesLowComplexityTail) {
    fq::processing::PolyTailTrimmer trimmer(fq::processing::PolyTailTrimmer::TailKind::PolyX, 4);

    fq::io::FastqRecord read{"read1", {}, "ACGTTTTT", "IIIIIIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACG");
    EXPECT_EQ(read.qual, "III");
}

TEST(PipelineSmokeTest, LengthTrimmerFromStartKeepsSuffix) {
    fq::processing::LengthTrimmer trimmer(3,
                                          fq::processing::LengthTrimmer::TrimStrategy::FromStart);

    fq::io::FastqRecord read{"read1", {}, "ACGT", "IIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "CGT");
    EXPECT_EQ(read.qual, "III");
}

TEST(PipelineSmokeTest, LengthTrimmerMaxLengthKeepsPrefix) {
    fq::processing::LengthTrimmer trimmer(3,
                                          fq::processing::LengthTrimmer::TrimStrategy::MaxLength);

    fq::io::FastqRecord read{"read1", {}, "ACGT", "IIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACG");
    EXPECT_EQ(read.qual, "III");
}

TEST(PipelineSmokeTest, FastqRecordValidationAllowsOnlyNonEmptyMatchingLengths) {
    fq::io::FastqRecord valid{"read1", {}, "ACGT", "IIII", "+"};
    fq::io::FastqRecord mismatched{"read2", {}, "ACGT", "III", "+"};
    fq::io::FastqRecord empty{"read3", {}, {}, {}, "+"};

    EXPECT_TRUE(valid.validateLengths());
    EXPECT_FALSE(mismatched.validateLengths());
    EXPECT_FALSE(empty.validateLengths());
}

TEST(PipelineSmokeTest, FastqBatchClearResetsStoredData) {
    fq::io::FastqBatch batch(1024, 2);
    batch.buffer().assign({'A', 'C', 'G', 'T'});
    batch.records().push_back(fq::io::FastqRecord{"read1", {}, "ACGT", "IIII", "+"});

    batch.clear();

    EXPECT_TRUE(batch.buffer().empty());
    EXPECT_TRUE(batch.records().empty());
}

