#include "fqtools/processing/mutators/quality_trimmer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

#include <fmt/format.h>

#ifdef __AVX2__
#include <immintrin.h>
#endif

namespace fq::processing {

// --- QualityTrimmer ---

QualityTrimmer::QualityTrimmer(double qualityThreshold,
                               size_t minLength,
                               TrimMode mode,
                               int qualityEncoding)
    : qualityThreshold_(qualityThreshold),
      minLength_(minLength),
      trimMode_(mode),
      qualityEncoding_(qualityEncoding) {}

auto QualityTrimmer::process(fq::io::FastqRecord& read) -> bool {
    if (read.empty()) {
        return false;
    }

    size_t originalLen = read.seq.size();
    size_t start = 0;
    size_t end = originalLen;

    // Trim 5'
    if (trimMode_ == TrimMode::Both || trimMode_ == TrimMode::FivePrime) {
        start = trimFivePrime(read.seq, read.qual);
    }

    // Trim 3'
    if (trimMode_ == TrimMode::Both || trimMode_ == TrimMode::ThreePrime) {
        // Only trim 3' if we still have bases left
        if (start < end) {
            std::string_view currentSeq = read.seq.substr(start, end - start);
            std::string_view currentQual = read.qual.substr(start, end - start);

            size_t newLen = trimThreePrime(currentSeq, currentQual);
            end = start + newLen;
        }
    }

    size_t newLen = (end > start) ? (end - start) : 0;

    // Check min length
    if (newLen < minLength_) {
        // Filter out (make empty)；走到这里 read 非空，必然发生了修改
        read.seq = {};
        read.qual = {};
        return true;
    }

    // Apply trim
    if (newLen < originalLen) {
        read.seq = read.seq.substr(start, newLen);
        read.qual = read.qual.substr(start, newLen);
        return true;
    }
    return false;
}

auto QualityTrimmer::trimFivePrime(std::string_view sequence, std::string_view quality) const
    -> size_t {
    size_t len = std::min(sequence.size(), quality.size());
    size_t i = 0;

#ifdef __AVX2__
    // 必须与标量路径 isHighQuality() 语义一致：q >= threshold（q 为整数），
    // 等价于 char >= ceil(threshold) + encoding。历史实现用 static_cast<int>
    // 截断阈值（floor），使小数阈值在 AVX2 构建与标量构建下剪出不同结果。
    const int target = static_cast<int>(std::ceil(qualityThreshold_)) + qualityEncoding_;
    if (target > 127) {
        // 任何 char 都无法达到门限：q = char - encoding <= 127 - encoding < threshold，
        // 全剪（同时规避 set1_epi8 的符号回绕）
        return len;
    }
    if (target - 1 >= -128) {
        // 找第一个高质量碱基：char >= target ⟺ char > target - 1。
        // _mm256_cmpgt_epi8 是有符号比较；target-1 落在 [-128, 126] 时，
        // 对全部字节取值都与标量整数比较一致。
        const __m256i vTarget = _mm256_set1_epi8(static_cast<char>(target - 1));
        for (; i + 32 <= len; i += 32) {
            __m256i chunk =
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(quality.data() + i));
            __m256i result = _mm256_cmpgt_epi8(chunk, vTarget);
            int mask = _mm256_movemask_epi8(result);
            if (mask != 0) {
                return i + static_cast<size_t>(__builtin_ctz(static_cast<unsigned>(mask)));
            }
        }
    }
    // target - 1 < -128 仅在阈值极低时出现（CLI 拒绝负值）；
    // 跳过 SIMD，由下方标量循环保证语义一致
#endif

    for (; i < len; ++i) {
        if (isHighQuality(quality[i])) {
            return i;
        }
    }
    return len;  // Trim all
}

auto QualityTrimmer::trimThreePrime(std::string_view sequence, std::string_view quality) const
    -> size_t {
    size_t len = std::min(sequence.size(), quality.size());
    // Scan from end
    size_t i = len;
    while (i > 0) {
        if (isHighQuality(quality[i - 1])) {
            return i;
        }
        i--;
    }
    return 0;  // Trim all
}

auto QualityTrimmer::isHighQuality(char qualityChar) const -> bool {
    // 显式按 int8_t 解释：非法字节（>=128）在 x86/ARM 上一致视为负质量
    int q = static_cast<int>(static_cast<std::int8_t>(qualityChar)) - qualityEncoding_;
    return q >= qualityThreshold_;
}

auto QualityTrimmer::getName() const -> std::string {
    return "QualityTrimmer";
}
auto QualityTrimmer::getDescription() const -> std::string {
    return fmt::format("Trims bases with quality < {:.2f}", qualityThreshold_);
}
// --- LengthTrimmer ---

LengthTrimmer::LengthTrimmer(size_t targetLength, TrimStrategy strategy)
    : targetLength_(targetLength), strategy_(strategy) {}

auto LengthTrimmer::process(fq::io::FastqRecord& read) -> bool {
    const size_t len = read.seq.size();
    if (len <= targetLength_) {
        return false;
    }
    // MaxLength: 从 3' 端截断，保留前 N 个碱基；
    // FromStart: 从 5' 端截断，保留末尾 N 个碱基
    const size_t start = (strategy_ == TrimStrategy::FromStart) ? (len - targetLength_) : 0;
    read.seq = read.seq.substr(start, targetLength_);
    read.qual = read.qual.substr(start, targetLength_);
    return true;
}

auto LengthTrimmer::getName() const -> std::string {
    return "LengthTrimmer";
}
auto LengthTrimmer::getDescription() const -> std::string {
    return fmt::format("Trims reads to length {}", targetLength_);
}
// --- AdapterTrimmer ---

AdapterTrimmer::AdapterTrimmer(const std::vector<std::string>& adapterSequences,
                               size_t minOverlap,
                               size_t maxMismatches)
    : adapters_(adapterSequences),
      minOverlap_(std::max<size_t>(1, minOverlap)),
      maxMismatches_(maxMismatches) {
    // 最小重叠必须严格大于允许错配数，否则重叠比对退化为必然命中：
    // 1 字符 overlap + 1 错配下，任何 read 的 3' 端都会被"匹配"而静默截断
    if (maxMismatches_ >= minOverlap_) {
        throw std::invalid_argument("adapter maxMismatches must be < minOverlap");
    }
}

auto AdapterTrimmer::process(fq::io::FastqRecord& read) -> bool {
    if (read.empty()) {
        return false;
    }

    // 只检查 3' 端锚定的 adapter，取最早命中的起点（保留最长插入片段）
    size_t bestPos = std::string::npos;  // Position to trim from (smallest index)

    for (const auto& adapter : adapters_) {
        size_t pos = findAdapter(read.seq, adapter);
        if (pos != std::string::npos) {
            if (bestPos == std::string::npos || pos < bestPos) {
                bestPos = pos;
            }
        }
    }

    if (bestPos != std::string::npos) {
        // Trim everything from bestPos
        read.seq = read.seq.substr(0, bestPos);
        read.qual = read.qual.substr(0, bestPos);
        return true;
    }
    return false;
}

auto AdapterTrimmer::findAdapter(std::string_view sequence, std::string_view adapter) const
    -> size_t {
    // 只识别锚定 3' 末端的 adapter：完整贴端匹配或尾部悬出 read 末尾的部分重叠。
    // 刻意不做全文内部搜索——read 中部偶然出现的 adapter 子序列（短 adapter、
    // poly-A 类周期序列尤其容易命中）会把后续真实插入片段静默剪掉（数据损失）。
    // 完整贴端匹配同样由下方重叠循环覆盖（overlapLen == adLen、mismatches == 0）。
    if (adapter.empty() || sequence.size() < minOverlap_ || adapter.size() < minOverlap_) {
        return std::string::npos;
    }

    const size_t seqLen = sequence.size();
    const size_t adLen = adapter.size();

    // 起点 i 的合法窗口：adapter 贴端（i = seqLen - adLen，若 adapter 不长于 read）
    // 到最小重叠下限（i = seqLen - minOverlap_）；i 越靠后 adapter 悬出越多
    const size_t startCheck = (seqLen > adLen) ? (seqLen - adLen) : 0;
    const size_t lastStart = seqLen - minOverlap_;

    for (size_t i = startCheck; i <= lastStart; ++i) {
        const size_t overlapLen = seqLen - i;
        if (countMismatches(sequence.substr(i), adapter.substr(0, overlapLen)) <= maxMismatches_) {
            return i;
        }
    }

    return std::string::npos;
}

auto AdapterTrimmer::countMismatches(std::string_view seq1, std::string_view seq2) const -> size_t {
    size_t len = std::min(seq1.size(), seq2.size());
    // Adjust inputs to be same length if passed different views?
    // The usage above ensures we compare suffix of seq1 vs prefix of seq2 of same length.
    // But safe to iterate min length.

    size_t mis = 0;
    for (size_t i = 0; i < len; ++i) {
        if (seq1[i] != seq2[i]) {  // Case sensitive? Usually yes for adapters
            mis++;
            if (mis > maxMismatches_) {
                return mis;
            }
        }
    }
    return mis;
}

auto AdapterTrimmer::getName() const -> std::string {
    return "AdapterTrimmer";
}
auto AdapterTrimmer::getDescription() const -> std::string {
    return "Trims adapter sequences";
}
// --- PolyTailTrimmer ---

PolyTailTrimmer::PolyTailTrimmer(TailKind kind, size_t minRunLength)
    : kind_(kind), minRunLength_(std::max<size_t>(1, minRunLength)) {}

auto PolyTailTrimmer::process(fq::io::FastqRecord& read) -> bool {
    if (read.empty()) {
        return false;
    }

    const size_t trimPos = trimPosition(read.seq);
    if (trimPos >= read.seq.size()) {
        return false;
    }

    read.seq = read.seq.substr(0, trimPos);
    read.qual = read.qual.substr(0, trimPos);
    return true;
}

auto PolyTailTrimmer::getName() const -> std::string {
    return kind_ == TailKind::PolyG ? "PolyGTailTrimmer" : "PolyXTailTrimmer";
}

auto PolyTailTrimmer::getDescription() const -> std::string {
    return kind_ == TailKind::PolyG
        ? fmt::format("Trims polyG tails with run >= {}", minRunLength_)
        : fmt::format("Trims low-complexity polyX tails with run >= {}", minRunLength_);
}

auto PolyTailTrimmer::trimPosition(std::string_view sequence) const -> size_t {
    if (sequence.size() < minRunLength_) {
        return sequence.size();
    }

    char tailBase = '\0';
    size_t runLength = 0;
    for (size_t i = sequence.size(); i > 0; --i) {
        const char normalized = normalizeBase(sequence[i - 1]);
        if (normalized == '\0') {
            break;
        }

        if (tailBase == '\0') {
            tailBase = normalized;
            runLength = 1;
        } else if (normalized == tailBase) {
            ++runLength;
        } else {
            break;
        }
    }

    if (runLength < minRunLength_) {
        return sequence.size();
    }

    if (kind_ == TailKind::PolyG && tailBase != 'G') {
        return sequence.size();
    }

    return sequence.size() - runLength;
}

auto PolyTailTrimmer::normalizeBase(char base) -> char {
    switch (base) {
        case 'A':
        case 'a':
            return 'A';
        case 'C':
        case 'c':
            return 'C';
        case 'G':
        case 'g':
            return 'G';
        case 'T':
        case 't':
            return 'T';
        default:
            return '\0';
    }
}

}  // namespace fq::processing
