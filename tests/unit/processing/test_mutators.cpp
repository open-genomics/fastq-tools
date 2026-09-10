/**
 * @file test_mutators.cpp
 * @brief Mutator 组件单元测试
 * @details 测试所有 Mutator 实现的功能正确性和边界条件
 */

#include "fqtools/io/fastq_io.h"
#include "fqtools/processing/mutators.h"

#include <gtest/gtest.h>

using namespace fq::processing;
using namespace fq::io;

// ============================================================================
// QualityTrimmer 测试
// ============================================================================

class QualityTrimmerTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

TEST_F(QualityTrimmerTest, TrimsLowQualityFromBothEnds) {
    QualityTrimmer trimmer(20.0);

    // '!' = 0, 'I' = 40
    FastqRecord read{"read1", {}, "ACGTACGT", "!!IIII!!", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "GTAC");
    EXPECT_EQ(read.qual, "IIII");
}

TEST_F(QualityTrimmerTest, TrimsOnlyFivePrime) {
    QualityTrimmer trimmer(20.0, 1, QualityTrimmer::TrimMode::FivePrime);

    FastqRecord read{"read1", {}, "ACGT", "!!II", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "GT");
    EXPECT_EQ(read.qual, "II");
}

TEST_F(QualityTrimmerTest, TrimsOnlyThreePrime) {
    QualityTrimmer trimmer(20.0, 1, QualityTrimmer::TrimMode::ThreePrime);

    FastqRecord read{"read1", {}, "ACGT", "II!!", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "AC");
    EXPECT_EQ(read.qual, "II");
}

TEST_F(QualityTrimmerTest, DropsReadShorterThanMinLength) {
    QualityTrimmer trimmer(20.0, 5);  // 最小长度 5

    FastqRecord read{"read1", {}, "ACGT", "!!II", "+"};
    trimmer.process(read);

    EXPECT_TRUE(read.seq.empty());
    EXPECT_TRUE(read.qual.empty());
}

TEST_F(QualityTrimmerTest, KeepsReadMeetingMinLength) {
    QualityTrimmer trimmer(20.0, 2);  // 最小长度 2

    FastqRecord read{"read1", {}, "ACGT", "!!II", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq.size(), 2);
}

TEST_F(QualityTrimmerTest, SupportsPhred64Encoding) {
    QualityTrimmer trimmer(20.0, 1, QualityTrimmer::TrimMode::Both, 64);

    // Phred+64: '@' = 0, '^' = 30
    FastqRecord read{"read1", {}, "ACGT", "@^^^", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "CGT");
    EXPECT_EQ(read.qual, "^^^");
}

TEST_F(QualityTrimmerTest, NoTrimWhenAllHighQuality) {
    QualityTrimmer trimmer(20.0);

    FastqRecord read{"read1", {}, "ACGT", "IIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACGT");
    EXPECT_EQ(read.qual, "IIII");
}

TEST_F(QualityTrimmerTest, DropsAllWhenAllLowQuality) {
    QualityTrimmer trimmer(20.0);

    FastqRecord read{"read1", {}, "ACGT", "!!!!", "+"};
    trimmer.process(read);

    EXPECT_TRUE(read.seq.empty());
    EXPECT_TRUE(read.qual.empty());
}

TEST_F(QualityTrimmerTest, HandlesEmptyRead) {
    QualityTrimmer trimmer(20.0);

    FastqRecord read{"read1", {}, {}, {}, "+"};
    trimmer.process(read);

    EXPECT_TRUE(read.seq.empty());
}

TEST_F(QualityTrimmerTest, GetNameReturnsNonEmpty) {
    QualityTrimmer trimmer(20.0);

    EXPECT_FALSE(trimmer.getName().empty());
}

TEST_F(QualityTrimmerTest, GetDescriptionReturnsNonEmpty) {
    QualityTrimmer trimmer(20.0);

    EXPECT_FALSE(trimmer.getDescription().empty());
}

// 回归：小数阈值在 SIMD（AVX2）与标量构建下必须语义一致。
// q >= 20.5 对整数 q 等价于 q >= 21（ceil 语义）；历史 AVX2 路径按 floor
// 截断阈值，会保留 q=20 的碱基，而标量构建剪掉它。
TEST_F(QualityTrimmerTest, FractionalThresholdUsesCeilSemantics) {
    QualityTrimmer trimmer(20.5, 1, QualityTrimmer::TrimMode::FivePrime);

    // Phred+33: '5' = q20, '6' = q21
    FastqRecord read{"read1", {}, "ACGTACGT", "55556666", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACGT");
    EXPECT_EQ(read.qual, "6666");
}

// 长 qual 串（>=32 字符）覆盖 SIMD 循环路径，小数阈值行为与标量一致
TEST_F(QualityTrimmerTest, FractionalThresholdConsistentOnSimdLengthInput) {
    QualityTrimmer trimmer(20.5, 1, QualityTrimmer::TrimMode::FivePrime);

    // FastqRecord 字段为 string_view：必须用命名存储，不能传临时 string
    const std::string low(36, '5');  // q20 × 36
    const std::string high(4, '6');  // q21 × 4
    const std::string seq(40, 'A');
    const std::string qual = low + high;
    FastqRecord read{"read1", {}, seq, qual, "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq.size(), 4U);
    EXPECT_EQ(read.qual, "6666");
}

// 整数阈值不受 SIMD 路径影响（长串覆盖循环）
TEST_F(QualityTrimmerTest, IntegerThresholdUnchangedOnSimdLengthInput) {
    QualityTrimmer trimmer(20.0, 1, QualityTrimmer::TrimMode::Both);

    const std::string low(33, '5');  // q20 × 33：整数阈值 20 下通过
    const std::string tail(7, '!');  // q0 × 7
    const std::string seq(40, 'A');
    const std::string qual = low + tail;
    FastqRecord read{"read1", {}, seq, qual, "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq.size(), 33U);
}

// 回归：Phred+64 下极端阈值（encoding + threshold > 127）必须全剪；
// 历史 AVX2 路径 set1_epi8 符号回绕，反而完全不剪
TEST_F(QualityTrimmerTest, Phred64ExtremeThresholdTrimsAll) {
    QualityTrimmer trimmer(70.0, 1, QualityTrimmer::TrimMode::FivePrime, 64);

    // '~' = 126 = Phred+64 下 q62，全部低于阈值；长度 >= 32 覆盖 SIMD 路径
    const std::string seq(40, 'A');
    const std::string qual(40, '~');
    FastqRecord read{"read1", {}, seq, qual, "+"};
    trimmer.process(read);

    EXPECT_TRUE(read.seq.empty());
    EXPECT_TRUE(read.qual.empty());
}

// ============================================================================
// LengthTrimmer 测试
// ============================================================================

class LengthTrimmerTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

TEST_F(LengthTrimmerTest, MaxLengthTrimsToExactLength) {
    LengthTrimmer trimmer(3, LengthTrimmer::TrimStrategy::MaxLength);

    FastqRecord read{"read1", {}, "ACGTACGT", "IIIIIIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq.size(), 3);
    EXPECT_EQ(read.qual.size(), 3);
}

TEST_F(LengthTrimmerTest, FromStartKeepsSuffix) {
    LengthTrimmer trimmer(3, LengthTrimmer::TrimStrategy::FromStart);

    FastqRecord read{"read1", {}, "ACGT", "IIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "CGT");
    EXPECT_EQ(read.qual, "III");
}

TEST_F(LengthTrimmerTest, MaxLengthDoesNotExtend) {
    LengthTrimmer trimmer(10, LengthTrimmer::TrimStrategy::MaxLength);

    FastqRecord read{"read1", {}, "ACGT", "IIII", "+"};
    trimmer.process(read);

    // 不会扩展，保持原长度
    EXPECT_EQ(read.seq, "ACGT");
    EXPECT_EQ(read.qual, "IIII");
}

TEST_F(LengthTrimmerTest, MaxLengthTrimsIfExceeded) {
    LengthTrimmer trimmer(2, LengthTrimmer::TrimStrategy::MaxLength);

    FastqRecord read{"read1", {}, "ACGT", "IIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq.size(), 2);
}

TEST_F(LengthTrimmerTest, HandlesShorterRead) {
    LengthTrimmer trimmer(10, LengthTrimmer::TrimStrategy::MaxLength);

    FastqRecord read{"read1", {}, "ACGT", "IIII", "+"};
    trimmer.process(read);

    // 读取长度 4，目标 10，应保持原长度
    EXPECT_EQ(read.seq.size(), 4);
}

TEST_F(LengthTrimmerTest, HandlesEmptyRead) {
    LengthTrimmer trimmer(3, LengthTrimmer::TrimStrategy::MaxLength);

    FastqRecord read{"read1", {}, {}, {}, "+"};
    trimmer.process(read);

    EXPECT_TRUE(read.seq.empty());
}

// 回归（CI fuzz 发现）：seq 与 qual 不等长是合法输入——见
// tools/fuzz/fastq_mutator_fuzzer.cpp 中 fuzzLengthTrimmer 的说明，允许产出
// 不等长结果。而 string_view::substr 在 pos > size() 时抛 std::out_of_range：
// FromStart 下 start 由 seq.size() 推出，qual 为空/偏短时 pos 越界，
// 异常逃逸即 std::terminate。
TEST_F(LengthTrimmerTest, FromStartWithEmptyQualDoesNotThrow) {
    LengthTrimmer trimmer(3, LengthTrimmer::TrimStrategy::FromStart);

    FastqRecord read{"read1", {}, "ACGTACGT", {}, "+"};
    EXPECT_NO_THROW(trimmer.process(read));

    EXPECT_EQ(read.seq, "CGT");
    EXPECT_TRUE(read.qual.empty());
}

TEST_F(LengthTrimmerTest, FromStartWithShorterQualDoesNotThrow) {
    LengthTrimmer trimmer(3, LengthTrimmer::TrimStrategy::FromStart);

    FastqRecord read{"read1", {}, "ACGTACGT", "II", "+"};  // qual 短于 seq
    EXPECT_NO_THROW(trimmer.process(read));

    EXPECT_EQ(read.seq, "CGT");
    // pos 被夹紧到 qual.size()，产出不等长结果而非异常
    EXPECT_TRUE(read.qual.empty());
}

TEST_F(LengthTrimmerTest, GetNameReturnsNonEmpty) {
    LengthTrimmer trimmer(3);

    EXPECT_FALSE(trimmer.getName().empty());
}

// ============================================================================
// AdapterTrimmer 测试
// ============================================================================

class AdapterTrimmerTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

TEST_F(AdapterTrimmerTest, RemovesAdapterFromEnd) {
    AdapterTrimmer trimmer({"TTAA"}, 3, 0);

    FastqRecord read{"read1", {}, "ACGTTTAA", "IIIIIIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACGT");
    EXPECT_EQ(read.qual, "IIII");
}

TEST_F(AdapterTrimmerTest, LeavesReadUntouchedWhenNoAdapter) {
    AdapterTrimmer trimmer({"TTAA"}, 3, 0);

    FastqRecord read{"read1", {}, "ACGTACGT", "IIIIIIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACGTACGT");
    EXPECT_EQ(read.qual, "IIIIIIII");
}

TEST_F(AdapterTrimmerTest, RespectsMinOverlap) {
    AdapterTrimmer trimmer({"TTAA"}, 4, 0);  // 最小重叠 4

    // 只有 2 个碱基匹配，不满足最小重叠
    FastqRecord read{"read1", {}, "ACGTTA", "IIIIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACGTTA");
}

TEST_F(AdapterTrimmerTest, AllowsMismatches) {
    AdapterTrimmer trimmer({"TTAA"}, 3, 1);  // 允许 1 个错配

    // TTAA vs TTGA: 1 个错配
    FastqRecord read{"read1", {}, "ACGTTTGA", "IIIIIIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACGT");
}

TEST_F(AdapterTrimmerTest, HandlesMultipleAdapters) {
    AdapterTrimmer trimmer({"TTAA", "GGCC"}, 3, 0);

    FastqRecord read1{"read1", {}, "ACGTTTAA", "IIIIIIII", "+"};
    trimmer.process(read1);
    EXPECT_EQ(read1.seq, "ACGT");

    FastqRecord read2{"read2", {}, "ACGTGGCC", "IIIIIIII", "+"};
    trimmer.process(read2);
    EXPECT_EQ(read2.seq, "ACGT");
}

TEST_F(AdapterTrimmerTest, HandlesEmptyRead) {
    AdapterTrimmer trimmer({"TTAA"}, 3, 0);

    FastqRecord read{"read1", {}, {}, {}, "+"};
    trimmer.process(read);

    EXPECT_TRUE(read.seq.empty());
}

TEST_F(AdapterTrimmerTest, HandlesAdapterLongerThanRead) {
    AdapterTrimmer trimmer({"TTAATTAA"}, 3, 0);

    FastqRecord read{"read1", {}, "ACGT", "IIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACGT");
}

TEST_F(AdapterTrimmerTest, HandlesMinOverlapLongerThanReadWithoutHanging) {
    AdapterTrimmer trimmer({"TTAA"}, 5, 0);

    FastqRecord read{"read1", {}, "ACGT", "IIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACGT");
    EXPECT_EQ(read.qual, "IIII");
}

TEST_F(AdapterTrimmerTest, GetNameReturnsNonEmpty) {
    AdapterTrimmer trimmer({"TTAA"}, 3, 0);

    EXPECT_FALSE(trimmer.getName().empty());
}

// 回归：内部完整匹配（后面还有真实序列）只可能来自偶然命中，
// 3' 端锚定契约下不得截断，否则静默丢弃真实插入片段
TEST_F(AdapterTrimmerTest, PreservesInternalAdapterMatch) {
    AdapterTrimmer trimmer({"TTAA"}, 3, 0);

    FastqRecord read{"read1", {}, "TTAACGTACGT", "IIIIIIIIIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "TTAACGTACGT");
    EXPECT_EQ(read.qual, "IIIIIIIIIII");
}

// adapter 尾部悬出 read 末尾的部分重叠：按重叠处截断
TEST_F(AdapterTrimmerTest, TrimsPartialOverhangAtThreePrimeEnd) {
    AdapterTrimmer trimmer({"TTAATTAA"}, 4, 0);

    // read 以 adapter 的前 6 个碱基结尾（TTAATT），尾部两个碱基悬出
    FastqRecord read{"read1", {}, "ACGTTAATT", "IIIIIIIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACG");
    EXPECT_EQ(read.qual, "III");
}

// read 整体是 adapter 前缀（贴端匹配的极端情形）：清空 read
TEST_F(AdapterTrimmerTest, EmptiesReadEntirelyMadeOfAdapter) {
    AdapterTrimmer trimmer({"TTAATTAA"}, 3, 0);

    FastqRecord read{"read1", {}, "TTAATT", "IIIIII", "+"};
    trimmer.process(read);

    EXPECT_TRUE(read.seq.empty());
    EXPECT_TRUE(read.qual.empty());
}

// process() 返回值契约：实际改动返回 true，未改动返回 false（管道 modifiedReads 依赖）
TEST_F(AdapterTrimmerTest, ProcessReportsWhetherRecordChanged) {
    AdapterTrimmer trimmer({"TTAA"}, 3, 0);

    FastqRecord modified{"read1", {}, "ACGTTTAA", "IIIIIIII", "+"};
    EXPECT_TRUE(trimmer.process(modified));

    FastqRecord untouched{"read2", {}, "ACGTACGT", "IIIIIIII", "+"};
    EXPECT_FALSE(trimmer.process(untouched));
}


// 回归：最小重叠 0 时 1 字符 overlap + 1 错配必然命中，任何 read 都会被剪掉 3' 端。
// 库层契约：不允许 0 最小重叠（CLI 侧另有 >=1 校验）。
TEST_F(AdapterTrimmerTest, RejectsZeroMinOverlap) {
    EXPECT_THROW(AdapterTrimmer({"TTAA"}, 0, 1), std::invalid_argument);
}

// 回归：允许错配数 >= 最小重叠时重叠比对退化为必然命中（空语义），拒绝该配置。
// 默认 minOverlap=3, maxMismatches=1 合法。
TEST_F(AdapterTrimmerTest, RejectsMismatchesAtLeastMinOverlap) {
    EXPECT_THROW(AdapterTrimmer({"TTAA"}, 3, 3), std::invalid_argument);
    EXPECT_THROW(AdapterTrimmer({"TTAA"}, 1, 1), std::invalid_argument);
    EXPECT_NO_THROW(AdapterTrimmer({"TTAA"}, 3, 1));
    EXPECT_NO_THROW(AdapterTrimmer({"TTAA"}, 2, 1));
}


// 非法质量字节（>=128）按 int8_t 语义视为负质量 → 低质量，各平台行为一致
// （x86 signed char 天然如此，ARM 需显式转换；与 AVX2 有符号比较一致）
TEST_F(QualityTrimmerTest, NonAsciiQualityByteTreatedAsLow) {
    QualityTrimmer trimmer(20.0);
    const std::string qual{static_cast<char>(0xff), static_cast<char>(0xff)};
    FastqRecord read{"read1", {}, "AC", qual, "+"};
    trimmer.process(read);
    EXPECT_TRUE(read.seq.empty());
}
class PolyTailTrimmerTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

TEST_F(PolyTailTrimmerTest, TrimsPolyGTailWhenRunLengthMet) {
    PolyTailTrimmer trimmer(PolyTailTrimmer::TailKind::PolyG, 4);

    FastqRecord read{"read1", {}, "ACGTGGGG", "IIIIIIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACGT");
    EXPECT_EQ(read.qual, "IIII");
}

TEST_F(PolyTailTrimmerTest, LeavesReadUntouchedWhenPolyGRunTooShort) {
    PolyTailTrimmer trimmer(PolyTailTrimmer::TailKind::PolyG, 5);

    FastqRecord read{"read1", {}, "ACGTGGGG", "IIIIIIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACGTGGGG");
    EXPECT_EQ(read.qual, "IIIIIIII");
}

TEST_F(PolyTailTrimmerTest, TrimsPolyXTailWhenTailIsLowComplexity) {
    PolyTailTrimmer trimmer(PolyTailTrimmer::TailKind::PolyX, 4);

    FastqRecord read{"read1", {}, "ACGTTTTT", "IIIIIIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACG");
    EXPECT_EQ(read.qual, "III");
}

TEST_F(PolyTailTrimmerTest, DoesNotTrimMixedTailInPolyXMode) {
    PolyTailTrimmer trimmer(PolyTailTrimmer::TailKind::PolyX, 4);

    FastqRecord read{"read1", {}, "ACGTTTTA", "IIIIIIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACGTTTTA");
    EXPECT_EQ(read.qual, "IIIIIIII");
}

// ============================================================================
// 边界条件测试
// ============================================================================

class MutatorBoundaryTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

TEST_F(MutatorBoundaryTest, SingleBaseRead) {
    QualityTrimmer qualityTrimmer(20.0);
    LengthTrimmer lengthTrimmer(1, LengthTrimmer::TrimStrategy::MaxLength);

    FastqRecord read1{"read1", {}, "A", "I", "+"};
    qualityTrimmer.process(read1);
    EXPECT_EQ(read1.seq, "A");

    FastqRecord read2{"read2", {}, "A", "I", "+"};
    lengthTrimmer.process(read2);
    EXPECT_EQ(read2.seq, "A");
}

TEST_F(MutatorBoundaryTest, VeryLongSequence) {
    std::string longSeq(10000, 'A');
    std::string longQual(10000, 'I');

    FastqRecord read{"read1", {}, longSeq, longQual, "+"};

    QualityTrimmer trimmer(20.0);
    trimmer.process(read);

    // 高质量序列应保持原长度
    EXPECT_EQ(read.seq.size(), 10000);
}

TEST_F(MutatorBoundaryTest, QualityThresholdAtExactValue) {
    // '5' = 53 - 33 = 20
    QualityTrimmer trimmer(20.0);

    FastqRecord read{"read1", {}, "AAAA", "5555", "+"};
    trimmer.process(read);

    // 质量等于阈值，应保留
    EXPECT_EQ(read.seq, "AAAA");
}

TEST_F(MutatorBoundaryTest, MixedQualityPattern) {
    QualityTrimmer trimmer(20.0);

    // 复杂质量模式（低高低…）：5' 端 '!' 被剪，3' 端 'I' 保留
    FastqRecord read{"read1", {}, "ACGTACGT", "!II!II!I", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "CGTACGT");
    EXPECT_EQ(read.qual, "II!II!I");
}

TEST_F(MutatorBoundaryTest, AdapterAtStart) {
    AdapterTrimmer trimmer({"ACGT"}, 4, 0);

    // 回归：adapter 位于 read 内部（非 3' 端锚定）时不得截断——
    // 旧实现做全文 find()，会把位置 0 起的匹配连同后续真实序列一起剪掉
    FastqRecord read{"read1", {}, "ACGTTTAA", "IIIIIIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACGTTTAA");
    EXPECT_EQ(read.qual, "IIIIIIII");
}

TEST_F(MutatorBoundaryTest, MultipleAdaptersWithPriority) {
    // 多个接头，应匹配第一个找到的
    AdapterTrimmer trimmer({"TTAA", "TTAA"}, 3, 0);

    FastqRecord read{"read1", {}, "ACGTTTAA", "IIIIIIII", "+"};
    trimmer.process(read);

    EXPECT_EQ(read.seq, "ACGT");
}

// ============================================================================
// 组合测试
// ============================================================================

class MutatorCompositionTest : public ::testing::Test {
protected:
    void SetUp() override {}
};

TEST_F(MutatorCompositionTest, QualityThenLengthTrimmer) {
    QualityTrimmer qualityTrimmer(20.0);
    LengthTrimmer lengthTrimmer(2, LengthTrimmer::TrimStrategy::MaxLength);

    FastqRecord read{"read1", {}, "ACGTACGT", "!!IIII!!", "+"};

    // 先质量修剪
    qualityTrimmer.process(read);
    EXPECT_EQ(read.seq, "GTAC");

    // 再长度修剪
    lengthTrimmer.process(read);
    EXPECT_EQ(read.seq, "GT");
}

TEST_F(MutatorCompositionTest, AdapterThenQualityTrimmer) {
    AdapterTrimmer adapterTrimmer({"TTAA"}, 3, 0);
    QualityTrimmer qualityTrimmer(20.0);

    // 接头在末尾，质量中间低两边高
    FastqRecord read{"read1", {}, "ACGTTTAA", "IIIIIIII", "+"};

    // 先接头去除
    adapterTrimmer.process(read);
    EXPECT_EQ(read.seq, "ACGT");

    // 接头已去除，序列全是高质量
    qualityTrimmer.process(read);
    EXPECT_EQ(read.seq, "ACGT");
}
