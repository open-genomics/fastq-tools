#include "statistics/statistics_report.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <map>
#include <sstream>
#include <string_view>
#include <vector>

#include "statistics/fq_statistic.h"

namespace fq::statistics {

namespace {

constexpr int kQ20Threshold = 20;
constexpr int kQ30Threshold = 30;

auto calculateErrorPerPosition(const uint64_t* qualSlot, uint64_t totalBases) -> double {
    if (totalBases == 0) {
        return 0.0;
    }

    double sumProb = 0.0;
    for (int q = 0; q < kMaxQual; ++q) {
        sumProb +=
            static_cast<double>(qualSlot[q]) * std::pow(10.0, -static_cast<double>(q) / 10.0);
    }
    return 100.0 * sumProb / static_cast<double>(totalBases);
}

auto sortedTopEntries(const std::map<std::string, uint64_t, std::less<>>& counts, size_t limit)
    -> std::vector<std::pair<std::string, uint64_t>> {
    std::vector<std::pair<std::string, uint64_t>> entries(counts.begin(), counts.end());
    const auto compare = [](const auto& lhs, const auto& rhs) {
        if (lhs.second != rhs.second) {
            return lhs.second > rhs.second;
        }
        return lhs.first < rhs.first;
    };

    if (entries.size() > limit) {
        std::partial_sort(entries.begin(),
                          entries.begin() + static_cast<std::ptrdiff_t>(limit),
                          entries.end(),
                          compare);
        entries.resize(limit);
    } else {
        std::sort(entries.begin(), entries.end(), compare);
    }
    return entries;
}

auto estimateDuplicates(uint64_t duplicateSampledReads, size_t sampleModulo) -> uint64_t {
    return duplicateSampledReads * std::max<size_t>(1, sampleModulo);
}

[[nodiscard]] auto fixedStream() -> std::ostringstream {
    std::ostringstream s;
    s << std::fixed << std::setprecision(2);
    return s;
}

auto formatMetricLine(const std::string& name, uint64_t count, uint64_t totalBases) -> std::string {
    auto line = fixedStream();
    const double ratio = totalBases == 0
        ? 0.0
        : 100.0 * static_cast<double>(count) / static_cast<double>(totalBases);
    line << name << '\t' << count << '\t' << ratio << '%';
    return line.str();
}

struct ComputedMetrics {
    std::string name;
    int phredQual = 33;
    uint64_t readCount = 0;
    uint64_t duplicateEstimate = 0;
    double duplicateEstimateRate = 0.0;
    uint64_t maxReadLength = 0;
    uint64_t totalBases = 0;
    uint64_t nQ20 = 0;
    uint64_t nQ30 = 0;
    uint64_t nA = 0;
    uint64_t nC = 0;
    uint64_t nG = 0;
    uint64_t nT = 0;
    uint64_t nN = 0;

    struct Position {
        uint64_t a = 0;
        uint64_t c = 0;
        uint64_t g = 0;
        uint64_t t = 0;
        uint64_t n = 0;
        double avgQual = 0.0;
        double errRate = 0.0;
        bool hasReads = false;
    };
    std::vector<Position> positions;
};

auto computeMetrics(const FqStatisticResult& result, const StatisticsWriterOptions& options)
    -> ComputedMetrics {
    ComputedMetrics metrics;
    metrics.name = std::filesystem::path(options.inputFastqPath).filename().string();
    metrics.phredQual = options.qualityEncoding;
    metrics.readCount = result.readCount;
    metrics.maxReadLength = result.maxReadLength;
    metrics.totalBases = result.totalBases;
    metrics.duplicateEstimate =
        estimateDuplicates(result.duplicateSampledReads, options.duplicateEstimateSampleModulo);
    if (result.readCount > 0) {
        metrics.duplicateEstimateRate = 100.0 * static_cast<double>(metrics.duplicateEstimate) /
            static_cast<double>(result.readCount);
    }

    for (size_t i = 0; i < result.maxReadLength; ++i) {
        const uint64_t* qSlot = result.qualityAt(i);
        const uint64_t* bSlot = result.baseAt(i);

        for (int j = kQ20Threshold; j < kMaxQual; ++j) {
            metrics.nQ20 += qSlot[j];
        }
        for (int j = kQ30Threshold; j < kMaxQual; ++j) {
            metrics.nQ30 += qSlot[j];
        }

        metrics.nA += bSlot[0];
        metrics.nC += bSlot[1];
        metrics.nG += bSlot[2];
        metrics.nT += bSlot[3];
        metrics.nN += bSlot[4];

        uint64_t sumQual = 0;
        uint64_t countReadsAtPos = 0;
        for (int j = 0; j < kMaxQual; ++j) {
            sumQual += qSlot[j] * static_cast<uint64_t>(j);
            countReadsAtPos += qSlot[j];
        }

        ComputedMetrics::Position pos;
        pos.a = bSlot[0];
        pos.c = bSlot[1];
        pos.g = bSlot[2];
        pos.t = bSlot[3];
        pos.n = bSlot[4];
        if (countReadsAtPos > 0) {
            pos.hasReads = true;
            pos.avgQual = static_cast<double>(sumQual) / static_cast<double>(countReadsAtPos);
            pos.errRate = calculateErrorPerPosition(qSlot, countReadsAtPos);
        }
        metrics.positions.push_back(pos);
    }

    return metrics;
}

auto jsonEscape(std::string_view value) -> std::string {
    std::string out;
    out.reserve(value.size() + 8);
    for (const char ch : value) {
        switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                // RFC 8259 要求 U+0000..U+001F 全部转义，其余控制字符用 \uXXXX
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                    out += buffer;
                } else {
                    out += ch;
                }
                break;
        }
    }
    return out;
}

auto formatCountPercent(uint64_t count, uint64_t totalBases) -> std::string {
    auto line = fixedStream();
    const double percent = totalBases == 0
        ? 0.0
        : 100.0 * static_cast<double>(count) / static_cast<double>(totalBases);
    line << "{\"count\": " << count << ", \"percent\": " << percent << '}';
    return line.str();
}

}  // namespace

auto buildStatisticsReport(const FqStatisticResult& result, const StatisticsWriterOptions& options)
    -> StatisticsReport {
    StatisticsReport report;
    if (result.readCount == 0) {
        return report;
    }

    const auto metrics = computeMetrics(result, options);

    report.summaryLines.push_back("#Name\t" + metrics.name);
    report.summaryLines.push_back("#PhredQual\t" + std::to_string(metrics.phredQual));
    report.summaryLines.push_back("#ReadNum\t" + std::to_string(metrics.readCount));
    report.summaryLines.push_back("#DuplicateEstimate\t" +
                                  std::to_string(metrics.duplicateEstimate));
    {
        auto line = fixedStream();
        line << "#DuplicateEstimateRate\t" << metrics.duplicateEstimateRate << '%';
        report.summaryLines.push_back(line.str());
    }
    report.summaryLines.push_back("#MaxReadLength\t" + std::to_string(metrics.maxReadLength));
    report.summaryLines.push_back("#BaseCount\t" + std::to_string(metrics.totalBases));
    report.summaryLines.push_back(formatMetricLine("#Q20(>=20)", metrics.nQ20, metrics.totalBases));
    report.summaryLines.push_back(formatMetricLine("#Q30(>=30)", metrics.nQ30, metrics.totalBases));
    report.summaryLines.push_back(formatMetricLine("#A", metrics.nA, metrics.totalBases));
    report.summaryLines.push_back(formatMetricLine("#C", metrics.nC, metrics.totalBases));
    report.summaryLines.push_back(formatMetricLine("#G", metrics.nG, metrics.totalBases));
    report.summaryLines.push_back(formatMetricLine("#T", metrics.nT, metrics.totalBases));
    report.summaryLines.push_back(formatMetricLine("#N", metrics.nN, metrics.totalBases));
    report.summaryLines.push_back(
        formatMetricLine("#GC", metrics.nG + metrics.nC, metrics.totalBases));

    report.positionLines.emplace_back("#Pos\tA\tC\tG\tT\tN\tAvgQual\tErrRate");
    for (size_t i = 0; i < metrics.positions.size(); ++i) {
        const auto& pos = metrics.positions[i];
        auto line = fixedStream();
        line << i + 1 << '\t' << pos.a << '\t' << pos.c << '\t' << pos.g << '\t' << pos.t << '\t'
             << pos.n << '\t';
        if (pos.hasReads) {
            line << pos.avgQual << '\t' << pos.errRate;
        } else {
            line << "0.0\t0.0";
        }
        report.positionLines.push_back(line.str());
    }

    report.signatureLines.emplace_back("metric\tkey\tcount");
    report.signatureLines.push_back("summary\ttotal_reads\t" + std::to_string(metrics.readCount));
    report.signatureLines.push_back("summary\tduplicate_estimate\t" +
                                    std::to_string(metrics.duplicateEstimate));
    for (const auto& [kmer, count] :
         sortedTopEntries(result.headKmerCounts, options.maxReportedSignatures)) {
        report.signatureLines.push_back("head_kmer\t" + kmer + '\t' + std::to_string(count));
    }

    return report;
}

auto formatStatisticsJson(const FqStatisticResult& result, const StatisticsWriterOptions& options)
    -> std::string {
    const auto metrics = computeMetrics(result, options);
    auto json = fixedStream();
    json << "{\n";
    json << R"(  "name": ")" << jsonEscape(metrics.name) << R"(",)" << "\n";
    json << "  \"phred_qual\": " << metrics.phredQual << ",\n";
    json << "  \"read_num\": " << metrics.readCount << ",\n";
    json << "  \"duplicate_estimate\": " << metrics.duplicateEstimate << ",\n";
    json << "  \"duplicate_estimate_rate\": " << metrics.duplicateEstimateRate << ",\n";
    json << "  \"max_read_length\": " << metrics.maxReadLength << ",\n";
    json << "  \"base_count\": " << metrics.totalBases << ",\n";
    json << "  \"q20\": " << formatCountPercent(metrics.nQ20, metrics.totalBases) << ",\n";
    json << "  \"q30\": " << formatCountPercent(metrics.nQ30, metrics.totalBases) << ",\n";
    json << "  \"bases\": {\n";
    json << "    \"A\": " << formatCountPercent(metrics.nA, metrics.totalBases) << ",\n";
    json << "    \"C\": " << formatCountPercent(metrics.nC, metrics.totalBases) << ",\n";
    json << "    \"G\": " << formatCountPercent(metrics.nG, metrics.totalBases) << ",\n";
    json << "    \"T\": " << formatCountPercent(metrics.nT, metrics.totalBases) << ",\n";
    json << "    \"N\": " << formatCountPercent(metrics.nN, metrics.totalBases) << ",\n";
    json << "    \"GC\": " << formatCountPercent(metrics.nG + metrics.nC, metrics.totalBases)
         << "\n";
    json << "  },\n";
    json << "  \"positions\": [";
    for (size_t i = 0; i < metrics.positions.size(); ++i) {
        const auto& pos = metrics.positions[i];
        if (i == 0) {
            json << '\n';
        } else {
            json << ",\n";
        }
        json << "    {\"pos\": " << (i + 1) << ", \"A\": " << pos.a << ", \"C\": " << pos.c
             << ", \"G\": " << pos.g << ", \"T\": " << pos.t << ", \"N\": " << pos.n
             << ", \"avg_qual\": " << pos.avgQual << ", \"err_rate\": " << pos.errRate << '}';
    }
    if (!metrics.positions.empty()) {
        json << '\n';
    }
    json << "  ]\n";
    json << "}\n";
    return json.str();
}

}  // namespace fq::statistics
