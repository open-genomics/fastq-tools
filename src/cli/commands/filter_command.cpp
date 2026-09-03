/**
 * @file filter_command.cpp
 * @brief filter 子命令实现
 * @details 过滤和修剪 FASTQ 文件
 *
 */

#include "filter_command.h"

#include <iostream>
#include <stdexcept>

#include "common_options.h"
#include "filter_plan.h"
#include <cxxopts.hpp>

#include <fqtools/error/error.h>
#include <fqtools/fq.h>  // 公共 API Façade（包含 pipeline 接口、predicates、mutators）
#include <fqtools/logging.h>
#include <fqtools/statistics/interfaces.h>

namespace fq::cli::commands {

FilterCommand::FilterCommand() = default;

FilterCommand::~FilterCommand() = default;

auto FilterCommand::execute(int argc, char* argv[]) -> int {
    cxxopts::Options options(getName(), getDescription());

    // 1. 添加共享参数
    CommonCliOptions::addOptions(options);

    // 2. 添加 filter 特有参数
    addFilterPlanOptions(options);
    options.add_options()(
        "stat", "Write TSV QC report for kept reads", cxxopts::value<std::string>())(
        "stat-json", "Write JSON QC report for kept reads", cxxopts::value<std::string>())(
        "h,help", "Print usage");

    if (argc == 1) {
        std::cout << options.help() << '\n';
        return 0;
    }

    auto result = options.parse(argc, argv);

    if (result.count("help")) {
        std::cout << options.help() << '\n';
        return 0;
    }

    // 3. 解析共享参数
    auto common = CommonCliOptions::parse(result);
    if (common.inputPath.empty() || common.outputPath.empty()) {
        throw std::invalid_argument(
            "both --input and --output options are required for the filter command");
    }

    auto plan = buildFilterPlan(result, common);
    plan.applyTo(pipeline_);

    const bool wantStat = result.count("stat") > 0 || result.count("stat-json") > 0;
    fq::statistics::StatisticOptions statOpts;
    if (wantStat) {
        statOpts.inputFastqPath = common.inputPath;
        statOpts.qualityEncoding =
            fq::cli::validateQualityEncoding(result["quality-encoding"].as<int>());
        if (result.count("stat")) {
            statOpts.outputStatPath = result["stat"].as<std::string>();
        }
        if (result.count("stat-json")) {
            statOpts.jsonOutputPath = result["stat-json"].as<std::string>();
        }
        if (common.outputPath == "-" &&
            (statOpts.outputStatPath == "-" || statOpts.jsonOutputPath == "-")) {
            throw fq::error::ConfigurationError(
                "cannot write FASTQ and a QC report to stdout; choose one '-' destination");
        }
        // 报告与 FASTQ 输入/输出别名会在 rename 发布时互相覆盖，参数阶段提前拒绝
        // （与输入的冲突在 writeStatisticsOutputs 仍有库层兜底）
        validateReportTargets(common.inputPath,
                              common.outputPath,
                              {statOpts.outputStatPath, statOpts.jsonOutputPath});
        // kmer/modulo 与报告外推共用 statOpts 同一来源，
        // 避免两处独立默认值未来暴露 CLI 参数时失同步
        pipeline_.enableReadStatistics(statOpts.qualityEncoding,
                                       statOpts.signatureKmerSize,
                                       statOpts.duplicateEstimateSampleModulo);
    }

    auto stats = pipeline_.run();
    if (wantStat) {
        fq::statistics::writeStatisticsOutputs(statOpts, pipeline_.readStatistics());
    }
    // stdout 已被 FASTQ 或 QC 报告占用时，人类可读摘要不得混入字节流
    // （下游 FASTQ 消费者会把摘要解析为格式错误），此时静默抑制
    const bool reportToStdout = statOpts.outputStatPath == "-" || statOpts.jsonOutputPath == "-" ||
        common.outputPath == "-";
    if (!reportToStdout && fq::logging::getLevel() < fq::logging::Level::Error) {
        std::cout << stats.toString() << '\n';
    }

    return 0;
}

auto FilterCommand::getName() const -> std::string {
    return "filter";
}

auto FilterCommand::getDescription() const -> std::string {
    return "Filter and trim FastQ files";
}

}  // namespace fq::cli::commands
