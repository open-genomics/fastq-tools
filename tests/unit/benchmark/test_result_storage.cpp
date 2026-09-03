#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include "fixture_loader.h"

#include "benchmark/result_storage.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace fq::benchmark {
namespace {

auto makeMetadata(const std::string& commit) -> SystemMetadata {
    SystemMetadata metadata;
    metadata.timestamp = "2026-08-30T12:34:56Z";
    metadata.gitCommit = commit;
    metadata.gitBranch = "master";
    metadata.cpuModel = "test-cpu";
    metadata.coreCount = 8;
    metadata.memoryBytes = 16ULL * 1024 * 1024 * 1024;
    metadata.osVersion = "test-os";
    metadata.compilerVersion = "test-compiler";
    return metadata;
}

auto makeReport(const std::string& commit, double meanTimeNs) -> BenchmarkReport {
    BenchmarkReport report;
    report.metadata = makeMetadata(commit);

    BenchmarkResult result;
    result.name = "io_read";
    result.category = "io";
    result.iterations = 5;
    result.meanTimeNs = meanTimeNs;
    result.stdDevNs = 12.5;
    result.minTimeNs = 100.0;
    result.maxTimeNs = 200.0;
    result.throughputMBps = 512.0;
    result.throughputReadsPerSec = 123456.0;
    result.peakMemoryBytes = 4096;
    result.threadCount = 2;
    result.inputSize = 1000;
    report.results.push_back(result);
    return report;
}

class ResultStorageTest : public ::testing::Test {
protected:
    void SetUp() override {
        storage_ = std::make_unique<ResultStorage>(tempDir_.path());
    }

    fq::test::TempDirectory tempDir_{"result_storage_"};
    std::unique_ptr<ResultStorage> storage_;
};

TEST_F(ResultStorageTest, SaveResult_CreatesTimestampedJsonFile) {
    const auto report = makeReport("abc1234", 150.0);
    const auto savedPath = storage_->saveResult(report);

    EXPECT_EQ(savedPath.filename().string(), "2026-08-30_12-34-56_abc1234.json");
    ASSERT_TRUE(std::filesystem::exists(savedPath));

    std::ifstream in(savedPath);
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    EXPECT_EQ(content.front(), '{');
}

TEST_F(ResultStorageTest, SaveAndLoadResult_RoundTripsAllFields) {
    const auto report = makeReport("def5678", 250.5);
    const auto savedPath = storage_->saveResult(report);

    const auto loaded = storage_->loadResult(savedPath.filename().string());
    ASSERT_EQ(loaded.results.size(), report.results.size());

    const auto& original = report.results.front();
    const auto& restored = loaded.results.front();
    EXPECT_EQ(restored.name, original.name);
    EXPECT_EQ(restored.category, original.category);
    EXPECT_EQ(restored.iterations, original.iterations);
    EXPECT_DOUBLE_EQ(restored.meanTimeNs, original.meanTimeNs);
    EXPECT_DOUBLE_EQ(restored.stdDevNs, original.stdDevNs);
    EXPECT_DOUBLE_EQ(restored.minTimeNs, original.minTimeNs);
    EXPECT_DOUBLE_EQ(restored.maxTimeNs, original.maxTimeNs);
    EXPECT_DOUBLE_EQ(restored.throughputMBps, original.throughputMBps);
    EXPECT_DOUBLE_EQ(restored.throughputReadsPerSec, original.throughputReadsPerSec);
    EXPECT_EQ(restored.peakMemoryBytes, original.peakMemoryBytes);
    EXPECT_EQ(restored.threadCount, original.threadCount);
    EXPECT_EQ(restored.inputSize, original.inputSize);

    EXPECT_EQ(loaded.metadata.gitCommit, report.metadata.gitCommit);
    EXPECT_EQ(loaded.metadata.timestamp, report.metadata.timestamp);
    EXPECT_EQ(loaded.metadata.gitBranch, report.metadata.gitBranch);
    EXPECT_EQ(loaded.metadata.cpuModel, report.metadata.cpuModel);
    EXPECT_EQ(loaded.metadata.coreCount, report.metadata.coreCount);
    EXPECT_EQ(loaded.metadata.memoryBytes, report.metadata.memoryBytes);
    EXPECT_EQ(loaded.metadata.osVersion, report.metadata.osVersion);
    EXPECT_EQ(loaded.metadata.compilerVersion, report.metadata.compilerVersion);
}

TEST_F(ResultStorageTest, ListResults_FiltersNonJsonAndSorts) {
    storage_->saveResult(makeReport("aaa1111", 1.0));
    storage_->saveResult(makeReport("bbb2222", 2.0));
    // 非 .json 文件不应进入结果列表
    {
        std::ofstream notes(tempDir_.path() / "results" / "notes.txt");
        notes << "not a result";
    }

    const auto results = storage_->listResults();
    ASSERT_EQ(results.size(), 2U);
    EXPECT_EQ(results.front(), "2026-08-30_12-34-56_aaa1111.json");
    EXPECT_EQ(results.back(), "2026-08-30_12-34-56_bbb2222.json");
}

TEST_F(ResultStorageTest, GetLatestResult_ReturnsNewestAndThrowsOnEmpty) {
    EXPECT_THROW(static_cast<void>(storage_->getLatestResult()), std::runtime_error);

    storage_->saveResult(makeReport("aaa1111", 1.0));
    storage_->saveResult(makeReport("bbb2222", 2.0));
    EXPECT_EQ(storage_->getLatestResult().metadata.gitCommit, "bbb2222");
}

TEST_F(ResultStorageTest, Baseline_SaveLoadExistsListRoundTrip) {
    const auto report = makeReport("abc1234", 150.0);
    const auto baselinePath = storage_->saveBaseline(report, "v1");

    ASSERT_TRUE(std::filesystem::exists(baselinePath));
    EXPECT_TRUE(storage_->baselineExists("v1"));
    EXPECT_FALSE(storage_->baselineExists("missing"));

    const auto loaded = storage_->loadBaseline("v1");
    ASSERT_EQ(loaded.results.size(), 1U);
    EXPECT_EQ(loaded.results.front().name, "io_read");

    const auto baselines = storage_->listBaselines();
    ASSERT_EQ(baselines.size(), 1U);
    EXPECT_EQ(baselines.front(), "v1");

    EXPECT_THROW(static_cast<void>(storage_->loadBaseline("missing")), std::runtime_error);
}

TEST_F(ResultStorageTest, LoadResult_CorruptJson_ThrowsParseError) {
    {
        std::ofstream garbage(tempDir_.path() / "results" / "garbage.json");
        garbage << "{not valid json";
    }
    EXPECT_THROW(static_cast<void>(storage_->loadResult("garbage.json")),
                 nlohmann::json::parse_error);
}

}  // namespace
}  // namespace fq::benchmark
