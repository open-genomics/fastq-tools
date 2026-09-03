/**
 * @file fq_statistic.cpp
 * @brief FASTQ 统计计算器实现
 * @details Calculator::Impl 内部走 ExecutionRuntime + StatisticRuntimeAdapter，
 *          合并了原 FastqStatisticCalculator 的职责（消除中间类）。
 */

#include "statistics/fq_statistic.h"

#include "fqtools/error/error.h"
#include "fqtools/logging.h"
#include "fqtools/statistics/statistics_writer.h"

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

#include "io/path_alias.h"
#include "processing/execution_runtime.h"
#include "statistics/fq_statistic_worker.h"
#include <fmt/format.h>
#include <sys/stat.h>

namespace fq::statistics {

namespace {

class StatisticRuntimeAdapter {
public:
    using result_type = FqStatisticResult;

    StatisticRuntimeAdapter(int qualityEncoding,
                            size_t signatureKmerSize,
                            size_t duplicateEstimateSampleModulo)
        : qualityEncoding_(qualityEncoding),
          signatureKmerSize_(signatureKmerSize),
          duplicateEstimateSampleModulo_(duplicateEstimateSampleModulo) {}

    [[nodiscard]] auto makeResult() const -> result_type {
        return {};
    }

    [[nodiscard]] auto processBatch(const fq::io::FastqBatch& batch) const -> result_type {
        FqStatisticWorker worker(
            qualityEncoding_, signatureKmerSize_, duplicateEstimateSampleModulo_);
        return worker.calculateStats(batch);
    }

    void afterCommit(result_type& /*unused*/, std::uint64_t /*unused*/) const {}

    void merge(result_type& total, const result_type& partial) const {
        total += partial;
    }

private:
    int qualityEncoding_;
    size_t signatureKmerSize_;
    size_t duplicateEstimateSampleModulo_;
};

void writeAtomically(const std::string& target,
                     const std::function<void(std::ostream&)>& writeBody) {
    // 唯一后缀（pid+counter）：与 FastqWriter 临时文件命名一致，
    // 避免固定 .tmp 在多进程写同一目标时互相覆盖/发布对方的半成品
    static std::atomic<std::uint64_t> counter{0};
    const auto tmpPath = target + ".tmp-" + std::to_string(::getpid()) + "-" +
        std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    // 任意阶段抛异常（writeBody/flush/rename）都清理残留临时文件，避免磁盘遗留半截报告
    try {
        {
            std::ofstream writer(tmpPath);
            if (!writer) {
                throw fq::error::IOError(tmpPath, errno);
            }
            writeBody(writer);
            writer.flush();
            if (!writer.good()) {
                throw fq::error::IOError(target, EIO);
            }
        }  // writer 此处析构关闭，确保数据落盘后再 rename
        std::error_code renameError;
        std::filesystem::rename(tmpPath, target, renameError);
        if (renameError) {
            throw fq::error::IOError(target, renameError.value());
        }
    } catch (...) {
        std::error_code removeError;
        std::filesystem::remove(tmpPath, removeError);
        throw;
    }
}

// 目标是否为特殊文件（字符/块设备、FIFO、socket）：与 FastqWriter 同一判断，
// 这些目标不支持"同目录临时文件 + rename"的原子发布（/dev 下创建临时文件会
// Permission denied，对设备 rename 也无意义），直接顺序写入。
auto isSpecialFileTarget(const std::string& target) -> bool {
    struct stat st{};
    if (::stat(target.c_str(), &st) != 0) {
        return false;  // 不存在或 stat 失败 → 走原子路径，open 阶段报错
    }
    return S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) || S_ISFIFO(st.st_mode) ||
        S_ISSOCK(st.st_mode);
}

// 先写同目录临时文件、校验流状态、再原子 rename 发布——
// 与 FastqWriter 的发布协议一致：磁盘满等写失败抛 IOError，绝不产出半截报告
void writeToDestination(const std::string& target,
                        const std::function<void(std::ostream&)>& writeBody) {
    if (target == "-") {
        writeBody(std::cout);
        std::cout.flush();
        if (!std::cout) {
            throw fq::error::IOError(target, EIO);
        }
        return;
    }
    if (isSpecialFileTarget(target)) {
        std::ofstream out(target, std::ios::binary);
        if (!out) {
            throw fq::error::IOError(target, errno);
        }
        writeBody(out);
        out.flush();
        if (!out.good()) {
            throw fq::error::IOError(target, EIO);
        }
        return;
    }
    writeAtomically(target, writeBody);
}

}  // namespace

void writeStatisticsOutputs(const StatisticOptions& options, const FqStatisticResult& result) {
    // 任一输出目标为 '-' 都会写 stdout；多个 '-' 目标会把不同格式拼接在同一个流上
    // （如 JSON 后跟 TSV），下游无法解析。必须保证最多一个 '-' 目标。
    const size_t stdoutTargets = static_cast<size_t>(options.outputStatPath == "-") +
        static_cast<size_t>(options.jsonOutputPath == "-") +
        static_cast<size_t>(options.signatureReportPath == "-");
    if (stdoutTargets > 1) {
        throw fq::error::ConfigurationError(
            "at most one of --output/--json/--signature-report may be '-' (stdout)");
    }

    // 报告经"临时文件 + rename"发布，会整体覆盖同名目标：目标指向输入 FASTQ
    // （stat -i x -o x）或彼此重合时静默毁掉原文件，必须在写出前拒绝
    std::vector<const std::string*> fileTargets;
    for (const auto* path :
         {&options.outputStatPath, &options.jsonOutputPath, &options.signatureReportPath}) {
        if (!path->empty() && *path != "-") {
            fileTargets.push_back(path);
        }
    }
    for (const auto* target : fileTargets) {
        if (fq::io::pathsAlias(options.inputFastqPath, *target)) {
            throw fq::error::ConfigurationError(fmt::format(
                "report target '{}' aliases the input FASTQ '{}'; choose a different output path",
                *target,
                options.inputFastqPath));
        }
    }
    for (size_t i = 0; i < fileTargets.size(); ++i) {
        for (size_t j = i + 1; j < fileTargets.size(); ++j) {
            if (fq::io::pathsAlias(*fileTargets[i], *fileTargets[j])) {
                throw fq::error::ConfigurationError(
                    fmt::format("report targets '{}' and '{}' refer to the same file",
                                *fileTargets[i],
                                *fileTargets[j]));
            }
        }
    }

    StatisticsWriterOptions writerOptions;
    writerOptions.inputFastqPath = options.inputFastqPath;
    writerOptions.qualityEncoding = options.qualityEncoding;
    writerOptions.duplicateEstimateSampleModulo = options.duplicateEstimateSampleModulo;
    writerOptions.signatureReportPath = options.signatureReportPath;
    writerOptions.jsonOutputPath = options.jsonOutputPath;
    writerOptions.maxReportedSignatures = options.maxReportedSignatures;

    StatisticsWriter statsWriter(writerOptions);
    if (!options.outputStatPath.empty()) {
        writeToDestination(options.outputStatPath,
                           [&](std::ostream& writer) { statsWriter.write(writer, result); });
    }

    if (!options.jsonOutputPath.empty()) {
        writeToDestination(options.jsonOutputPath,
                           [&](std::ostream& writer) { statsWriter.writeJson(writer, result); });
    }

    if (!options.signatureReportPath.empty()) {
        writeToDestination(options.signatureReportPath, [&](std::ostream& writer) {
            statsWriter.writeSignature(writer, result);
        });
    }
}

class Calculator::Impl {
public:
    explicit Impl(StatisticOptions options) : options_(std::move(options)) {
        options_.processing.validate();
    }

    void run() {
        fq::logging::info("Starting FASTQ statistics generation for '{}' using execution runtime.",
                          options_.inputFastqPath);

        processing::ExecutionRuntime runtime;
        processing::ExecutionRuntimeRequest plan;
        plan.inputPath = options_.inputFastqPath;
        plan.options = options_.processing;

        auto outcome =
            runtime.execute(plan,
                            StatisticRuntimeAdapter{options_.qualityEncoding,
                                                    options_.signatureKmerSize,
                                                    options_.duplicateEstimateSampleModulo});
        auto finalResult = std::move(outcome.result);

        fq::logging::info("Execution runtime finished. Aggregated results from all batches.");
        writeStatisticsOutputs(options_, finalResult);
        fq::logging::info("Statistics report saved to '{}'", options_.outputStatPath);
    }

private:
    StatisticOptions options_;
};

Calculator::Calculator(StatisticOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
Calculator::~Calculator() = default;
Calculator::Calculator(Calculator&&) noexcept = default;
auto Calculator::operator=(Calculator&&) noexcept -> Calculator& = default;

void Calculator::run() {
    impl_->run();
}

}  // namespace fq::statistics
