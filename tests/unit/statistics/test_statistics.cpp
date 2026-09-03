#include "fqtools/error/error.h"
#include "fqtools/io/fastq_io.h"
#include "fqtools/statistics/interfaces.h"

#include <deque>
#include <string>
#include <vector>

#include "statistics/fq_statistic.h"
#include "statistics/fq_statistic_worker.h"
#include <gtest/gtest.h>

namespace fq::statistics {

TEST(FqStatisticResultTest, OperatorPlusEquals) {
    FqStatisticResult res1;
    res1.readCount = 10;
    res1.totalBases = 1000;
    res1.maxReadLength = 100;
    res1.posQualityDist.resize(100 * kMaxQual, 1);
    res1.posBaseDist.resize(100 * kMaxBaseNum, 2);

    FqStatisticResult res2;
    res2.readCount = 5;
    res2.totalBases = 500;
    res2.maxReadLength = 120;  // Longer reads
    res2.posQualityDist.resize(120 * kMaxQual, 3);
    res2.posBaseDist.resize(120 * kMaxBaseNum, 4);

    res1 += res2;

    EXPECT_EQ(res1.readCount, 15);
    EXPECT_EQ(res1.totalBases, 1500);
    EXPECT_EQ(res1.maxReadLength, 120);

    // Check combined distributions for first 100 positions (flat layout: pos * stride + slot)
    for (int i = 0; i < 100; ++i) {
        EXPECT_EQ(res1.qualityAt(i)[0], 4);  // 1 + 3
        EXPECT_EQ(res1.baseAt(i)[0], 6);     // 2 + 4
    }

    // Check distribution for positions 100-120 (only from res2)
    for (int i = 100; i < 120; ++i) {
        EXPECT_EQ(res1.qualityAt(i)[0], 3);
        EXPECT_EQ(res1.baseAt(i)[0], 4);
    }
}

TEST(FqStatisticResultTest, AggregatesDuplicateAndSignatureMaps) {
    FqStatisticResult left;
    left.duplicateSampledReads = 1;
    left.headKmerCounts["AAAA"] = 2;
    left.sampledSequenceHashes[1] = 2;

    FqStatisticResult right;
    right.duplicateSampledReads = 4;
    right.headKmerCounts["AAAA"] = 4;
    right.headKmerCounts["TTTT"] = 1;
    right.sampledSequenceHashes[1] = 5;

    left += right;

    EXPECT_EQ(left.duplicateSampledReads, 6);
    EXPECT_EQ(left.headKmerCounts["AAAA"], 6);
    EXPECT_EQ(left.headKmerCounts["TTTT"], 1);
    EXPECT_EQ(left.sampledSequenceHashes[1], 7);
}

TEST(FqStatisticWorkerTest, CalculateStats) {
    FqStatisticWorker worker(33);  // Sanger offset
    fq::io::FastqBatch batch;

    fq::io::FastqRecord rec1;
    rec1.id = "read1";
    rec1.seq = "ACGTN";   // length 5
    rec1.qual = "!!#$!";  // Qualities: 0, 0, 2, 3, 0 (offset 33)
    batch.records().push_back(rec1);

    fq::io::FastqRecord rec2;
    rec2.id = "read2";
    rec2.seq = "AAAAA";   // length 5
    rec2.qual = "IIIII";  // Quality: 40 (offset 33)
    batch.records().push_back(rec2);

    auto result = worker.calculateStats(batch);

    EXPECT_EQ(result.readCount, 2);
    EXPECT_EQ(result.totalBases, 10);
    EXPECT_EQ(result.maxReadLength, 5);

    // Check base distribution at pos 0: 1 A (from rec2), 1 A (from rec1) -> 2 A
    EXPECT_EQ(result.baseAt(0)[0], 2);  // A

    // Check N count at pos 4: 1 N (from rec1)
    EXPECT_EQ(result.baseAt(4)[4], 1);  // N

    // Check Quality distribution at pos 0: 1 '!' (0), 1 'I' (40)
    EXPECT_EQ(result.qualityAt(0)[0], 1);
    EXPECT_EQ(result.qualityAt(0)[40], 1);
}

TEST(FqStatisticWorkerTest, RespectsConfiguredQualityEncoding) {
    fq::io::FastqBatch batch;

    fq::io::FastqRecord rec;
    rec.id = "read1";
    rec.seq = "A";
    rec.qual = "@";
    batch.records().push_back(rec);

    FqStatisticWorker worker33(33);
    auto result33 = worker33.calculateStats(batch);
    EXPECT_EQ(result33.qualityAt(0)[31], 1);

    FqStatisticWorker worker64(64);
    auto result64 = worker64.calculateStats(batch);
    EXPECT_EQ(result64.qualityAt(0)[0], 1);
}

TEST(FqStatisticWorkerTest, EmptyBatch) {
    FqStatisticWorker worker;
    fq::io::FastqBatch batch;
    auto result = worker.calculateStats(batch);
    EXPECT_EQ(result.readCount, 0);
    EXPECT_TRUE(result.posQualityDist.empty());
}

TEST(FqStatisticWorkerTest, TracksDuplicatesAndHeadKmers) {
    FqStatisticWorker worker(33, 4, 1);
    fq::io::FastqBatch batch;

    fq::io::FastqRecord rec1{"read1", {}, "ACGTAAAA", "IIIIIIII", "+"};
    fq::io::FastqRecord rec2{"read2", {}, "ACGTAAAA", "IIIIIIII", "+"};
    fq::io::FastqRecord rec3{"read3", {}, "TTTTCCCC", "IIIIIIII", "+"};

    batch.records().push_back(rec1);
    batch.records().push_back(rec2);
    batch.records().push_back(rec3);

    const auto result = worker.calculateStats(batch);

    EXPECT_EQ(result.readCount, 3);
    EXPECT_EQ(result.duplicateSampledReads, 1);
    EXPECT_EQ(result.headKmerCounts.at("ACGT"), 2);
    EXPECT_EQ(result.headKmerCounts.at("TTTT"), 1);
}

// Space-Saving 淘汰语义：sketch 满时新 key 以"被替换者计数+1"进入，
// 而不是被丢弃、也不无语义地削减现有条目（回归：低频 kmer 丢失 + 计数低估）
TEST(FqStatisticWorkerTest, HeadKmerEvictionReplacesMinimumWithInheritedCount) {
    FqStatisticWorker worker(33, 8, 1);
    fq::io::FastqBatch batch;
    // FastqRecord 持有 string_view：seq/qual 必须存放于测试存续期内。
    // deque push_back 不搬移既有元素（引用/视图稳定），且 seq/qual 均持久化，
    // 避免临时 string 视图悬空（UB）与 vector 扩容搬移 SSO 串导致的视图失效
    std::deque<std::string> storage;
    const auto add = [&batch, &storage](std::string seq) {
        storage.push_back(std::move(seq));
        const auto& s = storage.back();
        storage.push_back(std::string(s.size(), 'I'));
        const auto& q = storage.back();
        batch.records().push_back({"r", {}, s, q, "+"});
    };

    // 填满 64 条目：63 个高计数(10) + 1 个最低计数 M(3)
    for (int i = 0; i < 63; ++i) {
        std::string k = "H";
        k += static_cast<char>('A' + i / 26);
        k += static_cast<char>('A' + i % 26);
        k += "CCCCC";
        for (int r = 0; r < 10; ++r) {
            add(k + "GGGGGG");
        }
    }
    for (int r = 0; r < 3; ++r) {
        add("MMMMMMMMGGGGGG");
    }

    // 新 key 首次出现：应以 min+1 = 4 替换 M 进入表
    add("XXXXXXXXGGGGGG");

    const auto result = worker.calculateStats(batch);
    EXPECT_EQ(result.headKmerCounts.size(), 64u);
    const auto x = result.headKmerCounts.find("XXXXXXXX");
    ASSERT_NE(x, result.headKmerCounts.end()) << "新 kmer 被淘汰逻辑丢弃";
    EXPECT_EQ(x->second, 4u) << "Space-Saving 应继承被替换者计数+1";
    EXPECT_EQ(result.headKmerCounts.count("MMMMMMMM"), 0u);
    EXPECT_EQ(result.headKmerCounts.at("HAACCCCC"), 10u);  // 高频条目不受影响
}

TEST(FqStatisticWorkerTest, HeadKmerEvictionAccumulatesRepeatedNewKey) {
    FqStatisticWorker worker(33, 8, 1);
    fq::io::FastqBatch batch;
    // 同上一用例：seq/qual 用 deque 持久化，避免 string_view 悬空
    std::deque<std::string> storage;
    const auto add = [&batch, &storage](std::string seq) {
        storage.push_back(std::move(seq));
        const auto& s = storage.back();
        storage.push_back(std::string(s.size(), 'I'));
        const auto& q = storage.back();
        batch.records().push_back({"r", {}, s, q, "+"});
    };

    for (int i = 0; i < 63; ++i) {
        std::string k = "H";
        k += static_cast<char>('A' + i / 26);
        k += static_cast<char>('A' + i % 26);
        k += "CCCCC";
        for (int r = 0; r < 10; ++r) {
            add(k + "GGGGGG");
        }
    }
    for (int r = 0; r < 3; ++r) {
        add("MMMMMMMMGGGGGG");
    }
    // X 出现 5 次：首次以 4 替换 M，随后 4 次递增 => 8
    for (int r = 0; r < 5; ++r) {
        add("XXXXXXXXGGGGGG");
    }

    const auto result = worker.calculateStats(batch);
    EXPECT_EQ(result.headKmerCounts.at("XXXXXXXX"), 8u);
}

// 回归: 多个输出目标同时为 '-'（stdout）时拼接会互相污染——JSON 后跟 TSV 导致不可解析。
// 历史实现只防护 "TSV + JSON" 一种组合, 漏掉 JSON/signature 与 signature/TSV 组合。
TEST(StatisticsWriterTest, RejectsMultipleStdoutDestinations) {
    FqStatisticResult result;
    result.readCount = 1;

    {
        fq::statistics::StatisticOptions opts;
        opts.outputStatPath = "-";
        opts.jsonOutputPath = "-";
        EXPECT_THROW(fq::statistics::writeStatisticsOutputs(opts, result),
                     fq::error::ConfigurationError);
    }
    {
        fq::statistics::StatisticOptions opts;
        opts.jsonOutputPath = "-";
        opts.signatureReportPath = "-";
        EXPECT_THROW(fq::statistics::writeStatisticsOutputs(opts, result),
                     fq::error::ConfigurationError);
    }
    {
        fq::statistics::StatisticOptions opts;
        opts.outputStatPath = "-";
        opts.signatureReportPath = "-";
        EXPECT_THROW(fq::statistics::writeStatisticsOutputs(opts, result),
                     fq::error::ConfigurationError);
    }
}

// 回归：报告目标指向输入 FASTQ（如 stat -i x -o x）会在写出阶段把输入文件
// 覆盖掉；库层在触碰任何文件之前拒绝该配置。别名经字符串归一化即可判定，
// 无需目标真实存在，因此这些用例不产生文件 I/O。
TEST(StatisticsWriterTest, RejectsReportTargetAliasingInputFastq) {
    FqStatisticResult result;

    fq::statistics::StatisticOptions same;
    same.inputFastqPath = "in.fastq";
    same.outputStatPath = "in.fastq";
    EXPECT_THROW(fq::statistics::writeStatisticsOutputs(same, result),
                 fq::error::ConfigurationError);

    fq::statistics::StatisticOptions normalized;
    normalized.inputFastqPath = "in.fastq";
    normalized.jsonOutputPath = "./in.fastq";
    EXPECT_THROW(fq::statistics::writeStatisticsOutputs(normalized, result),
                 fq::error::ConfigurationError);
}

// 回归：多份报告互为别名时，后写者覆盖先写者，只留下一份（或半份）报告
TEST(StatisticsWriterTest, RejectsDuplicateReportTargets) {
    FqStatisticResult result;

    fq::statistics::StatisticOptions opts;
    opts.inputFastqPath = "in.fastq";
    opts.outputStatPath = "report.tsv";
    opts.jsonOutputPath = "report.tsv";
    EXPECT_THROW(fq::statistics::writeStatisticsOutputs(opts, result),
                 fq::error::ConfigurationError);
}


// 回归：signatureKmerSize 为 0 时 substr(0,0) 会把空字符串塞进 headKmerCounts，
// 报告出现空 key 行。库层应把 kmer 大小钳制到 >= 1。
TEST(FqStatisticWorkerTest, ZeroSignatureKmerSizeClampedToAtLeastOne) {
    FqStatisticWorker worker(33, 0, 1);
    fq::io::FastqBatch batch;
    fq::io::FastqRecord rec{"read1", {}, "ACGT", "IIII", "+"};
    batch.records().push_back(rec);

    const auto result = worker.calculateStats(batch);

    EXPECT_EQ(result.headKmerCounts.count(""), 0u);
    EXPECT_EQ(result.headKmerCounts.at("A"), 1u);
}


// 质量字节 >= 128 是非法 FASTQ 输入；char 符号性随平台而异（x86 signed / ARM unsigned），
// 必须显式按 int8_t 解释，保证各平台统计一致（与 AVX2 的有符号比较语义一致）。
// 契约：0xFF 视为负质量 → clamp 到 bin 0，而非 ARM 上 unsigned char 的 q=222 → bin 41。
TEST(FqStatisticWorkerTest, NonAsciiQualityByteTreatedAsLowAcrossPlatforms) {
    FqStatisticWorker worker(33);
    fq::io::FastqBatch batch;
    const std::string qual{static_cast<char>(0xff)};
    const std::string seq{"A"};
    batch.records().push_back({"r", {}, seq, qual, "+"});

    const auto result = worker.calculateStats(batch);

    EXPECT_EQ(result.qualityAt(0)[0], 1u);
    EXPECT_EQ(result.qualityAt(0)[41], 0u);
}


}  // namespace fq::statistics
