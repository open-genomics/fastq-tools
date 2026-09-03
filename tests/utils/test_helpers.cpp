#include "test_helpers.h"

#include <fstream>
#include <random>
#include <sstream>

namespace fq::test {

std::vector<std::filesystem::path> TestDataGenerator::tempPaths_;

auto TestDataGenerator::createTempFile(const std::string& content, const std::string& suffix)
    -> std::filesystem::path {
    auto tempDir = std::filesystem::temp_directory_path();
    auto tempFile =
        tempDir / ("fastqtools_test_" + std::to_string(std::random_device{}()) + suffix);

    std::ofstream file(tempFile);
    file << content;
    file.close();

    tempPaths_.push_back(tempFile);
    return tempFile;
}

auto TestDataGenerator::generateFastQRecords(size_t count, size_t readLength) -> std::string {
    std::ostringstream oss;

    for (size_t i = 0; i < count; ++i) {
        oss << "@read_" << i << '\n';
        oss << generateRandomDNA(readLength) << '\n';
        oss << "+\n";
        oss << generateRandomQuality(readLength) << '\n';
    }

    return oss.str();
}

auto TestDataGenerator::generateRandomDNA(size_t length) -> std::string {
    static const char bases[] = "ATGC";
    thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> dis(0, 3);

    std::string sequence;
    sequence.reserve(length);

    for (size_t i = 0; i < length; ++i) {
        sequence += bases[dis(gen)];
    }

    return sequence;
}

auto TestDataGenerator::generateRandomQuality(size_t length, int minQuality, int maxQuality)
    -> std::string {
    thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<> dis(minQuality, maxQuality);

    std::string quality;
    quality.reserve(length);

    for (size_t i = 0; i < length; ++i) {
        quality += static_cast<char>(dis(gen) + 33);  // Phred+33 编码
    }

    return quality;
}

void TestDataGenerator::cleanup() {
    for (const auto& path : tempPaths_) {
        std::error_code ec;
        if (std::filesystem::is_directory(path)) {
            std::filesystem::remove_all(path, ec);
        } else {
            std::filesystem::remove(path, ec);
        }
    }
    tempPaths_.clear();
}

void FastQToolsTest::SetUp() {
    // 自当前目录向上定位 tests/data/fixtures（测试工作目录位于构建树内）
    auto dir = std::filesystem::current_path();
    for (int depth = 0; depth < 6 && !std::filesystem::exists(dir / "tests" / "data" / "fixtures");
         ++depth) {
        dir = dir / "..";
    }
    testDataDir_ = dir / "tests" / "data" / "fixtures";
}

void FastQToolsTest::TearDown() {
    TestDataGenerator::cleanup();
}

}  // namespace fq::test
