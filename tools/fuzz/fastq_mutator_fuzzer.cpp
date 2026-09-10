/**
 * @file fastq_mutator_fuzzer.cpp
 * @brief 真实 FASTQ 修改器模糊测试
 *
 * 直接调用 fq::processing 的 4 个真实 mutator：
 *   - QualityTrimmer（含 AVX2 SIMD 路径）
 *   - LengthTrimmer
 *   - AdapterTrimmer
 *   - PolyTailTrimmer
 *
 * 用 FuzzedDataProvider 拆 fuzz 数据为 seq/qual/mode/threshold 等参数，
 * 构造 FastqRecord（view 指向稳定 std::string 缓冲）并 process()，
 * 覆盖 substr 边界、SIMD 越界读、adapter find 下溢等真实路径。
 *
 * 构建：cmake -DENABLE_FUZZING=ON -DCMAKE_CXX_COMPILER=clang++ -B build-fuzz
 * 运行：./build-fuzz/fuzzers/fastq_mutator_fuzzer tools/fuzz/corpus/ -max_len=4096
 */

#include "fqtools/processing/mutators.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

namespace {

/// @brief 用稳定缓冲构造 FastqRecord，保证 view 生命周期安全
auto makeRecord(std::string seq, std::string qual, std::string& seqBuf, std::string& qualBuf)
    -> fq::io::FastqRecord {
    seqBuf = std::move(seq);
    qualBuf = std::move(qual);
    fq::io::FastqRecord rec;
    rec.id = std::string_view("fuzz_id");
    rec.seq = std::string_view(seqBuf);
    // 故意允许长度不匹配：mutator 应当不崩
    rec.qual = std::string_view(qualBuf);
    rec.plus = std::string_view("+");
    return rec;
}

/// @brief 质量分数字符空间（Phred+33 可打印 ASCII 33..126，再加几个非法字符测健壮性）
constexpr char kQualChars[] =
    "!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
    "abcdefghijklmnopqrstuvwxyz{|}~"
    "\x00\x01\x02\xff";  // 含非法字符，测 clamp 与负值路径

constexpr char kSeqChars[] = "ACGTNacgtnRYMKBDHVWSrysmkbdhvw\x00\xff-z.";

auto fuzzQuality(FuzzedDataProvider& dp, size_t len) -> std::string {
    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(dp.ConsumeBool() ? dp.ConsumeIntegralInRange<char>(0, 126)
                                     : kQualChars[dp.ConsumeIntegralInRange<size_t>(0, sizeof(kQualChars) - 2)]);
    }
    return s;
}

auto fuzzSequence(FuzzedDataProvider& dp, size_t len) -> std::string {
    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        s.push_back(kSeqChars[dp.ConsumeIntegralInRange<size_t>(0, sizeof(kSeqChars) - 2)]);
    }
    return s;
}

auto fuzzAdapter(FuzzedDataProvider& dp) -> std::vector<std::string> {
    std::vector<std::string> adapters;
    const auto count = dp.ConsumeIntegralInRange<size_t>(0, 3);
    for (size_t i = 0; i < count; ++i) {
        adapters.push_back(dp.ConsumeRandomLengthString(32));
    }
    return adapters;
}

/// @brief 跑 QualityTrimmer，覆盖 Both/FivePrime/ThreePrime 三种模式与各种 threshold
auto fuzzQualityTrimmer(FuzzedDataProvider& dp, const std::string& seqBuf, const std::string& qualBuf) -> void {
    using TM = fq::processing::QualityTrimmer::TrimMode;
    const auto modeSel = dp.ConsumeIntegralInRange<int>(0, 2);
    const auto mode = modeSel == 0 ? TM::Both : (modeSel == 1 ? TM::FivePrime : TM::ThreePrime);
    // threshold 覆盖极端：负、0、中、超 93
    const auto threshold = dp.ConsumeFloatingPointInRange<double>(-5.0, 100.0);
    const auto minLength = dp.ConsumeIntegralInRange<size_t>(0, 256);
    const auto encoding = dp.ConsumeIntegralInRange<int>(0, 64);

    fq::processing::QualityTrimmer trimmer(threshold, minLength, mode, encoding);

    std::string seqB;
    std::string qualB;
    auto rec = makeRecord(seqBuf, qualBuf, seqB, qualB);
    trimmer.process(rec);

    // 访问结果：触发 string_view 越界读
    volatile size_t newLen = rec.seq.size();
    volatile size_t qualLen = rec.qual.size();
    (void)newLen;
    (void)qualLen;
    if (!rec.seq.empty()) {
        volatile char front = rec.seq.front();
        (void)front;
    }
    if (!rec.qual.empty()) {
        volatile char back = rec.qual.back();
        (void)back;
    }
}

auto fuzzLengthTrimmer(FuzzedDataProvider& dp, const std::string& seqBuf, const std::string& qualBuf) -> void {
    using TS = fq::processing::LengthTrimmer::TrimStrategy;
    const auto stratSel = dp.ConsumeIntegralInRange<int>(0, 1);
    const auto strat = static_cast<TS>(stratSel);
    const auto target = dp.ConsumeIntegralInRange<size_t>(0, 512);

    fq::processing::LengthTrimmer trimmer(target, strat);

    std::string seqB;
    std::string qualB;
    auto rec = makeRecord(seqBuf, qualBuf, seqB, qualB);
    trimmer.process(rec);

    volatile size_t newLen = rec.seq.size();
    (void)newLen;
    // 不校验 seq/qual 等长：LengthTrimmer 基于 seq.size() 算 newLen，
    // 对不等长输入（合法 fuzz 用例）qual.substr 可能产生不等长结果，属预期
}

auto fuzzAdapterTrimmer(FuzzedDataProvider& dp, const std::string& seqBuf, const std::string& qualBuf) -> void {
    auto adapters = fuzzAdapter(dp);
    if (adapters.empty()) {
        return;  // AdapterTrimmer 构造允许空 vector，但无意义
    }
    // AdapterTrimmer 的构造函数有前置条件 maxMismatches < minOverlap（且 minOverlap
    // 被夹紧到 >=1），违反即抛 std::invalid_argument。此前这里独立生成两个区间，
    // 非法组合必然命中，异常逃逸到 libFuzzer 就是 std::terminate——
    // 那是 fuzzer 违反合法前置条件，不是产品缺陷。
    // 改为在合法域内生成：先定 minOverlap，再让 maxMismatches 严格小于它。
    const auto minOverlap = dp.ConsumeIntegralInRange<size_t>(1, 64);
    const auto maxMismatches = dp.ConsumeIntegralInRange<size_t>(0, minOverlap - 1);

    fq::processing::AdapterTrimmer trimmer(adapters, minOverlap, maxMismatches);

    std::string seqB;
    std::string qualB;
    auto rec = makeRecord(seqBuf, qualBuf, seqB, qualB);
    trimmer.process(rec);

    volatile size_t newLen = rec.seq.size();
    (void)newLen;
}

auto fuzzPolyTailTrimmer(FuzzedDataProvider& dp, const std::string& seqBuf, const std::string& qualBuf) -> void {
    using TK = fq::processing::PolyTailTrimmer::TailKind;
    const auto kind = dp.ConsumeBool() ? TK::PolyG : TK::PolyX;
    const auto minRun = dp.ConsumeIntegralInRange<size_t>(0, 64);

    fq::processing::PolyTailTrimmer trimmer(kind, minRun);

    std::string seqB;
    std::string qualB;
    auto rec = makeRecord(seqBuf, qualBuf, seqB, qualB);
    trimmer.process(rec);

    volatile size_t newLen = rec.seq.size();
    (void)newLen;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 64 * 1024) {
        return 0;
    }
    if (size < 2) {
        return 0;
    }

    FuzzedDataProvider dp(data, size);

    // 决定 seq/qual 是否等长（等长是合法 fastq；不等长是畸形，测健壮性）
    const auto seqLen = dp.ConsumeIntegralInRange<size_t>(0, 1024);
    const auto equalLen = dp.ConsumeBool();
    const auto qualLen = equalLen ? seqLen : dp.ConsumeIntegralInRange<size_t>(0, 1024);

    auto seqBuf = fuzzSequence(dp, seqLen);
    auto qualBuf = fuzzQuality(dp, qualLen);

    // 4 个 mutator 顺序消费同一 FuzzedDataProvider，共享 seq/qual 输入
    fuzzQualityTrimmer(dp, seqBuf, qualBuf);
    fuzzLengthTrimmer(dp, seqBuf, qualBuf);
    fuzzAdapterTrimmer(dp, seqBuf, qualBuf);
    fuzzPolyTailTrimmer(dp, seqBuf, qualBuf);

    return 0;
}
