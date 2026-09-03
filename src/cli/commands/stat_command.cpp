/**
 * @file stat_command.cpp
 * @brief stat 子命令实现
 * @details 生成 FASTQ 文件的统计信息
 *
 */

#include "stat_command.h"

#include <iostream>
#include <stdexcept>
#include <utility>

#include "common_options.h"
#include <cxxopts.hpp>

#include <fqtools/statistics/interfaces.h>

namespace fq::cli::commands {

auto StatCommand::execute(int argc, char* argv[]) -> int {
    cxxopts::Options options(getName(), getDescription());

    // 1. 添加共享参数
    CommonCliOptions::addOptions(options);

    // 2. 添加 stat 特有参数
    options.add_options()("signature-report",
                          "Write optional signature sidecar report (TSV)",
                          cxxopts::value<std::string>())(
        "signature-kmer-size",
        "Head k-mer size for signature report",
        cxxopts::value<size_t>()->default_value("15"))(
        "signature-limit",
        "Maximum signature rows to emit",
        cxxopts::value<size_t>()->default_value("20"))(
        "duplicate-sample-modulo",
        "Sampling modulo for duplicate estimation (1 = exact in tests)",
        cxxopts::value<size_t>()->default_value("1024"))(
        "quality-encoding",
        "Quality encoding offset (33 or 64)",
        cxxopts::value<int>()->default_value("33"))(
        "json", "Write machine-readable JSON report to this path", cxxopts::value<std::string>())(
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
        throw std::invalid_argument("both --input and --output options are required");
    }

    // 4. 构建选项
    fq::statistics::StatisticOptions statOpts;
    statOpts.processing = common.toProcessingOptions();
    statOpts.inputFastqPath = common.inputPath;
    statOpts.outputStatPath = common.outputPath;
    statOpts.qualityEncoding =
        fq::cli::validateQualityEncoding(result["quality-encoding"].as<int>());
    statOpts.signatureKmerSize = result["signature-kmer-size"].as<size_t>();
    if (statOpts.signatureKmerSize == 0) {
        throw std::invalid_argument("signature-kmer-size must be >= 1");
    }
    statOpts.maxReportedSignatures = result["signature-limit"].as<size_t>();
    statOpts.duplicateEstimateSampleModulo = result["duplicate-sample-modulo"].as<size_t>();

    if (result.count("signature-report")) {
        statOpts.signatureReportPath = result["signature-report"].as<std::string>();
    }
    if (result.count("json")) {
        statOpts.jsonOutputPath = result["json"].as<std::string>();
    }

    // stat 无 FASTQ 输出，报告目标只可能与输入文件或彼此别名；
    // 提前拒绝，避免跑完流水线才发现配置会覆盖数据
    validateReportTargets(
        common.inputPath,
        {},
        {statOpts.outputStatPath, statOpts.jsonOutputPath, statOpts.signatureReportPath});

    fq::statistics::Calculator calculator(std::move(statOpts));
    calculator.run();
    return 0;
}

auto StatCommand::getName() const -> std::string {
    return "stat";
}

auto StatCommand::getDescription() const -> std::string {
    return "Generate statistics for a FASTQ file";
}

}  // namespace fq::cli::commands
