#include "fqtools/error/error.h"
#include "fqtools/io/fastq_io.h"
#include "fqtools/io/fastq_reader.h"
#include "fqtools/io/fastq_writer.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include <gtest/gtest.h>
#include <sys/stat.h>

namespace fq::io {

class FastqWriterTest : public ::testing::Test {
protected:
    void SetUp() override {
        tmpFile_ = "test_writer_output.fastq";
    }

    void TearDown() override {
        if (std::filesystem::exists(tmpFile_)) {
            std::filesystem::remove(tmpFile_);
        }
    }

    std::string tmpFile_;
};

TEST_F(FastqWriterTest, WriteBasic) {
    {
        FastqWriter writer(tmpFile_);
        EXPECT_TRUE(writer.isOpen());

        FastqRecord rec;
        rec.id = "read1";
        rec.seq = "ACGT";
        rec.qual = "IIII";
        writer.write(rec);

        FastqRecord rec2;
        rec2.id = "read2";
        rec2.comment = "desc";
        rec2.seq = "AAAA";
        rec2.qual = "JJJJ";
        writer.write(rec2);

        writer.finish();
    }

    ASSERT_TRUE(std::filesystem::exists(tmpFile_));
    EXPECT_GT(std::filesystem::file_size(tmpFile_), 0);

    std::ifstream in(tmpFile_);
    ASSERT_TRUE(in.is_open());
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());

    EXPECT_NE(content.find("@read1\nACGT\n+\nIIII\n"), std::string::npos);
    EXPECT_NE(content.find("@read2 desc\nAAAA\n+\nJJJJ\n"), std::string::npos);
}

// 特征测试：默认构造记录（全空 string_view，data() 可能为 nullptr）不得崩溃，
// 且产出结构合法的 FASTQ（防御 memcpy(nullptr, 0) 的未定义行为）
// 输出文件权限应遵循 umask 惯例（0666 & ~umask），而不是固定 0600
TEST_F(FastqWriterTest, PublishedOutputFollowsUmask) {
    const mode_t originalUmask = ::umask(022);
    {
        FastqWriter writer(tmpFile_);
        FastqRecord rec;
        rec.id = "r";
        rec.seq = "ACGT";
        rec.qual = "IIII";
        writer.write(rec);
        writer.finish();
    }
    ::umask(originalUmask);

    struct stat st{};
    ASSERT_EQ(::stat(tmpFile_.c_str(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0644) << std::oct << "mode=" << (st.st_mode & 0777);
}

TEST_F(FastqWriterTest, WritesDefaultConstructedRecordWithoutCrashing) {
    {
        FastqWriter writer(tmpFile_);
        FastqRecord emptyRec;
        writer.write(emptyRec);
        writer.finish();
    }
    ASSERT_TRUE(std::filesystem::exists(tmpFile_));
    std::ifstream in(tmpFile_);
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "@\n\n+\n\n");
}

TEST_F(FastqWriterTest, BatchWriteReportsCommittedUncompressedBytesThroughContract) {
    FastqBatch batch;
    batch.records().push_back(FastqRecord{"read1", {}, "ACGT", "IIII", "+"});

    FastqWriter concreteWriter(tmpFile_);
    IWriter& writer = concreteWriter;

    EXPECT_EQ(writer.write(batch), 19U);
}

TEST_F(FastqWriterTest, PreservesCustomPlusLineWhenWriting) {
    {
        FastqWriter writer(tmpFile_);
        ASSERT_TRUE(writer.isOpen());

        FastqRecord rec;
        rec.id = "read1";
        rec.comment = "desc";
        rec.seq = "ACGT";
        rec.plus = "+read1 desc";
        rec.qual = "IIII";
        writer.write(rec);

        writer.finish();
    }

    std::ifstream in(tmpFile_);
    ASSERT_TRUE(in.is_open());
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());

    EXPECT_NE(content.find("@read1 desc\nACGT\n+read1 desc\nIIII\n"), std::string::npos);
}

TEST_F(FastqWriterTest, FinishPublishesOutputAtomically) {
    FastqRecord rec{"read1", {}, "ACGT", "IIII", "+"};
    {
        FastqWriter writer(tmpFile_);
        writer.write(rec);
        EXPECT_FALSE(std::filesystem::exists(tmpFile_));
        writer.finish();
        EXPECT_TRUE(std::filesystem::exists(tmpFile_));
    }

    std::ifstream in(tmpFile_);
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "@read1\nACGT\n+\nIIII\n");
}

TEST_F(FastqWriterTest, FinishClosesGzipStreamBeforePublishing) {
    tmpFile_ += ".gz";
    FastqWriterOptions options;
    options.compression = FastqWriterCompressionMode::Gzip;
    options.compressionLevel = 1;
    FastqRecord rec{"read1", {}, "ACGT", "IIII", "+"};

    {
        FastqWriter writer(tmpFile_, options);
        writer.write(rec);
        writer.finish();
    }

    FastqReader reader(tmpFile_);
    FastqBatch batch;
    ASSERT_TRUE(reader.nextBatch(batch));
    ASSERT_EQ(batch.size(), 1U);
    EXPECT_EQ(batch.records().front().seq, "ACGT");
    EXPECT_FALSE(reader.nextBatch(batch));
}

TEST_F(FastqWriterTest, FinishFailurePreservesExistingTarget) {
    std::filesystem::remove(tmpFile_);
    ASSERT_TRUE(std::filesystem::create_directory(tmpFile_));

    {
        FastqWriter writer(tmpFile_);
        FastqRecord rec{"read1", {}, "ACGT", "IIII", "+"};
        writer.write(rec);

        EXPECT_THROW(writer.finish(), fq::error::IOError);
        EXPECT_TRUE(std::filesystem::is_directory(tmpFile_));
    }
    std::filesystem::remove_all(tmpFile_);
}

TEST_F(FastqWriterTest, WritesToCharacterDeviceWithoutAtomicTemporary) {
    FastqWriter writer("/dev/null");
    EXPECT_TRUE(writer.isOpen());

    writer.write(FastqRecord{"read1", {}, "ACGT", "IIII", "+"});
    EXPECT_NO_THROW(writer.finish());
}

// 未 finish 即析构：只清理临时文件，绝不发布输出（IWriter 契约）
TEST_F(FastqWriterTest, DestructorWithoutFinishDoesNotPublish) {
    {
        FastqWriter writer(tmpFile_);
        FastqRecord rec{"read1", {}, "ACGT", "IIII", "+"};
        writer.write(rec);
        // 不调用 finish()，直接析构
    }

    EXPECT_FALSE(std::filesystem::exists(tmpFile_));
    for (const auto& entry : std::filesystem::directory_iterator(".")) {
        EXPECT_EQ(entry.path().filename().string().find("test_writer_output.fastq.tmp-"),
                  std::string::npos)
            << "临时文件残留: " << entry.path();
    }
}

// 异常展开场景：处理中途失败时，截断内容不得出现在目标路径
TEST_F(FastqWriterTest, ExceptionUnwindDoesNotPublishTruncatedOutput) {
    try {
        FastqWriter writer(tmpFile_);
        FastqRecord rec{"read1", {}, "ACGT", "IIII", "+"};
        writer.write(rec);
        throw std::runtime_error("simulated pipeline failure");
        // writer 在栈展开中析构，未经 finish()
    } catch (const std::runtime_error&) {
        // 预期异常
    }

    EXPECT_FALSE(std::filesystem::exists(tmpFile_));
    for (const auto& entry : std::filesystem::directory_iterator(".")) {
        EXPECT_EQ(entry.path().filename().string().find("test_writer_output.fastq.tmp-"),
                  std::string::npos)
            << "临时文件残留: " << entry.path();
    }
}

TEST_F(FastqWriterTest, WritesUncompressedStdoutDash) {
    const std::string captured = "test_writer_stdout.fastq";
    const int outFd = ::open(captured.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    ASSERT_GE(outFd, 0);
    const int savedStdout = ::dup(STDOUT_FILENO);
    ASSERT_GE(savedStdout, 0);
    ASSERT_EQ(::dup2(outFd, STDOUT_FILENO), STDOUT_FILENO);
    ::close(outFd);

    {
        FastqWriter writer("-");
        EXPECT_TRUE(writer.isOpen());
        writer.write(FastqRecord{"read1", {}, "ACGT", "IIII", "+"});
        writer.finish();
    }

    fflush(stdout);
    ::dup2(savedStdout, STDOUT_FILENO);
    ::close(savedStdout);

    std::ifstream in(captured);
    const std::string content((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    EXPECT_EQ(content, "@read1\nACGT\n+\nIIII\n");
    EXPECT_FALSE(std::filesystem::exists("-"));
    std::filesystem::remove(captured);
}

}  // namespace fq::io
