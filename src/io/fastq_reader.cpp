#include "fqtools/io/fastq_reader.h"

#include "fqtools/error/error.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <utility>

#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#include <fmt/format.h>

namespace fq::io {

struct FastqReader::Impl {
    gzFile gzfile = nullptr;
    int fd = -1;
    bool isGzip = false;
    bool ownsFd = true;
    std::string path;
    bool isEofReached = false;
    FastqReaderOptions options{};
    std::vector<char> remainder;
    // 动态学习的平均记录字节数（含全部四行与换行），用于本批目标读取量估算；
    // 初始 512B 为保守高估，按累计平均在首批后即贴近实际值，
    // 避免固定高估导致的系统性过读与 remainder 反复搬运
    double estimatedRecordBytes = 512.0;
    double learnedParsedBytes = 0.0;
    double learnedParsedRecords = 0.0;

    explicit Impl(std::string p, const FastqReaderOptions& opt) : path(std::move(p)), options(opt) {
        if (path == "-") {
            openStdin();
            return;
        }

        // 单次打开：sniff 与后续读取共用同一 fd，消除两次 open 之间
        // 文件被替换的 TOCTOU 窗口（与 FastqWriter 临时文件协议同理）
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            throw fq::error::IOError(path, errno);
        }

        unsigned char header[2] = {0, 0};
        size_t headerBytes = 0;
        while (headerBytes < sizeof(header)) {
            const auto n = ::read(fd, header + headerBytes, sizeof(header) - headerBytes);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                const int savedErrno = errno;
                ::close(fd);
                fd = -1;
                throw fq::error::IOError(path, savedErrno);
            }
            if (n == 0) {
                break;  // 文件不足 2 字节，按普通文件处理
            }
            headerBytes += static_cast<size_t>(n);
        }

        if (headerBytes == sizeof(header) && header[0] == 0x1f && header[1] == 0x8b) {
            isGzip = true;
            // gzdopen 从当前偏移解析 gzip 流，须先回退已消费的 sniff 字节；
            // 成功后 fd 所有权移交 gzFile
            if (::lseek(fd, 0, SEEK_SET) < 0) {
                const int savedErrno = errno;
                if (savedErrno == ESPIPE) {
                    // 管道/FIFO 等不可回退输入：gzdopen 无法从偏移 0 重新解析
                    // gzip 头（魔数已被消费），与 stdin 的 gzip 语义保持一致，
                    // 给出明确的配置错误而不是令人困惑的 "Illegal seek"
                    ::close(fd);
                    fd = -1;
                    throw fq::error::ConfigurationError(
                        "gzip-compressed input from a pipe/FIFO is not supported; "
                        "decompress first (e.g. gzip -dc file.gz | FastQTools ... -i -)");
                }
                ::close(fd);
                fd = -1;
                throw fq::error::IOError(path, savedErrno);
            }
            gzfile = gzdopen(fd, "r");
            if (!gzfile) {
                ::close(fd);
                fd = -1;
                throw fq::error::IOError(path, errno);
            }
            gzbuffer(gzfile, static_cast<unsigned>(options.zlibBufferBytes));
            return;
        }

        // 非 gzip：回退 sniff 已消费的字节后从头读取。
        // 管道/FIFO 不可 seek（ESPIPE）：把已消费的 sniff 字节存入 remainder，
        // 后续读取从剩余流继续（与 stdin 路径同一模式）。
        if (::lseek(fd, 0, SEEK_SET) < 0) {
            const int savedErrno = errno;
            if (savedErrno == ESPIPE) {
                if (headerBytes > 0) {
                    remainder.assign(reinterpret_cast<const char*>(header),
                                     reinterpret_cast<const char*>(header) + headerBytes);
                }
                return;
            }
            ::close(fd);
            fd = -1;
            throw fq::error::IOError(path, savedErrno);
        }

#ifdef __linux__
        // 提示内核进行顺序预读，显著提升大文件顺序读取性能
        ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
    }

    void openStdin() {
        fd = STDIN_FILENO;
        ownsFd = false;
        // 管道等读端可能只返回部分字节（短读）或被信号打断；
        // 必须循环凑满 2 字节魔数后再判 gzip，否则单字节短读会误判为普通文本，
        // 把应有的 CONFIG 错误退化成误导性的 FORMAT 错误。语义与文件路径嗅探一致。
        unsigned char header[2] = {0, 0};
        size_t headerBytes = 0;
        while (headerBytes < sizeof(header)) {
            const auto n = ::read(STDIN_FILENO, header + headerBytes, sizeof(header) - headerBytes);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw fq::error::IOError(path, errno);
            }
            if (n == 0) {
                break;  // EOF：不足 2 字节按普通文本处理
            }
            headerBytes += static_cast<size_t>(n);
        }

        if (headerBytes == sizeof(header) && header[0] == 0x1f && header[1] == 0x8b) {
            throw fq::error::ConfigurationError(
                "gzip-compressed stdin is not supported; decompress first (e.g. gzip -dc file.gz | "
                "FastQTools ... -i -)");
        }
        if (headerBytes > 0) {
            remainder.assign(reinterpret_cast<const char*>(header),
                             reinterpret_cast<const char*>(header) + headerBytes);
        }
    }

    ~Impl() {
        if (isGzip) {
            if (gzfile) {
                gzclose(gzfile);
            }
        } else if (ownsFd && fd >= 0) {
            ::close(fd);
        }
    }

    [[nodiscard]] auto isOpen() const -> bool {
        if (isGzip) {
            return gzfile != nullptr;
        }
        return fd >= 0;
    }

    auto readSome(char* dst, size_t toRead) const -> ssize_t {
        if (toRead == 0) {
            return 0;
        }
        if (isGzip) {
            // gzread 接受 unsigned int 参数：分块避免大缓冲静默截断（外层循环会续读）
            constexpr size_t kMaxGzChunk =
                static_cast<size_t>(std::numeric_limits<unsigned>::max()) / 2;
            const auto chunk = static_cast<unsigned>(std::min(toRead, kMaxGzChunk));
            const int n = gzread(gzfile, dst, chunk);
            return static_cast<ssize_t>(n);
        }

        while (true) {
            const auto n = ::read(fd, dst, toRead);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return n;
        }
    }

    static auto findEol(const char* ptr, const char* end) -> const char* {
        return static_cast<const char*>(std::memchr(ptr, '\n', static_cast<size_t>(end - ptr)));
    }
};

FastqReader::FastqReader(const std::string& path) : FastqReader(path, FastqReaderOptions{}) {}

FastqReader::FastqReader(const std::string& path, const FastqReaderOptions& options)
    : impl_(std::make_unique<Impl>(path, options)) {}

FastqReader::~FastqReader() = default;

FastqReader::FastqReader(FastqReader&&) noexcept = default;
FastqReader& FastqReader::operator=(FastqReader&&) noexcept = default;

auto FastqReader::isOpen() const -> bool {
    return impl_ && impl_->isOpen();
}

auto FastqReader::nextBatch(FastqBatch& batch, size_t maxRecords) -> bool {
    if (maxRecords == 0) {
        // 0 会使读取循环不读任何数据即报 EOF（或携 remainder 时空转），属调用方契约错误
        throw std::invalid_argument("FastqReader::nextBatch maxRecords must be >= 1");
    }
    if (!impl_ || !impl_->isOpen()) {
        return false;
    }

    batch.records().clear();
    batch.buffer().clear();

    // 将上批未消费的残片拷入 batch buffer 头部：保留对象池预分配的容量，
    // 避免 std::move(remainder) 把池缓冲替换成小存储后又反复重新分配
    if (!impl_->remainder.empty()) {
        batch.buffer().assign(impl_->remainder.begin(), impl_->remainder.end());
        impl_->remainder.clear();
    }

    if (batch.buffer().empty() && impl_->isEofReached) {
        return false;
    }

    while (true) {
        if (!impl_->isEofReached) {
            const size_t chunk = std::max<size_t>(1, impl_->options.readChunkBytes);
            const bool unlimited = (maxRecords == std::numeric_limits<size_t>::max());
            const size_t maxBuf = impl_->options.maxBufferBytes;

            size_t targetBytes = 0;
            if (unlimited) {
                targetBytes = batch.buffer().size() + chunk;
            } else {
                // 按动态平均记录大小估算目标字节数：固定高估会系统性过读，
                // 使每批读满缓冲上限后把大半数据存入 remainder、下批再整体拷回
                const double want = static_cast<double>(maxRecords) * impl_->estimatedRecordBytes;
                const auto buffered = static_cast<double>(batch.buffer().size());
                // 目标至少比当前缓冲大一个 chunk：当估算过小（单条记录超过
                // maxRecords × 平均记录长）且上次迭代未能解析出任何记录时，
                // 若目标仍等于当前大小，内层读取循环条件恒为假、不读任何字节，
                // 解析再次失败 → 外层死循环（挂起）。+chunk 保证总能取得新数据，
                // 直到记录完整或触及 maxBufferBytes 守卫
                targetBytes =
                    static_cast<size_t>(std::max(buffered + static_cast<double>(chunk), want));
            }

            if (maxBuf > 0) {
                targetBytes = std::min(targetBytes, maxBuf);
            }

            while (!impl_->isEofReached && batch.buffer().size() < targetBytes) {
                const auto kCurrentSize = batch.buffer().size();
                if (maxBuf > 0 && kCurrentSize >= maxBuf) {
                    break;
                }

                size_t toRead = chunk;
                if (maxBuf > 0) {
                    const auto kRemaining = maxBuf - kCurrentSize;
                    if (kRemaining == 0) {
                        break;
                    }
                    toRead = std::min(toRead, kRemaining);
                }

                if (batch.buffer().capacity() < kCurrentSize + chunk) {
                    const auto kNewCap =
                        std::max(batch.buffer().capacity() * 2, kCurrentSize + chunk);
                    batch.buffer().reserve(kNewCap);
                }

                batch.buffer().resize(kCurrentSize + toRead);
                const auto kBytesRead =
                    impl_->readSome(batch.buffer().data() + kCurrentSize, toRead);
                if (kBytesRead < 0) {
                    if (impl_->isGzip) {
                        int err = 0;
                        const char* msg = gzerror(impl_->gzfile, &err);
                        if (err == Z_ERRNO) {
                            throw fq::error::IOError(impl_->path, errno);
                        }
                        // 其余 Z_* 码不是 errno，直接携带 zlib 的错误描述，
                        // 避免 strerror() 把压缩库错误码渲染成无意义文本
                        throw fq::error::FastQException(fq::error::ErrorCategory::IO,
                                                        fq::error::ErrorSeverity::Critical,
                                                        msg ? msg : "gzread failed");
                    }
                    throw fq::error::IOError(impl_->path, errno);
                }
                batch.buffer().resize(kCurrentSize + static_cast<size_t>(kBytesRead));
                if (kBytesRead == 0) {
                    impl_->isEofReached = true;
                }
            }
        }

        if (batch.buffer().empty()) {
            return false;
        }

        const char* data = batch.buffer().data();
        const char* end = data + batch.buffer().size();
        const char* ptr = data;
        const char* lastValidPtr = ptr;

        while (ptr < end && batch.records().size() < maxRecords) {
            while (ptr < end && (*ptr == '\n' || *ptr == '\r')) {
                ++ptr;
            }
            if (ptr >= end) {
                break;
            }

            if (*ptr != '@') {
                // Robustness check
                // If we are here, we expect a record start.
                // If EOF is reached, this loop should have terminated if we handle trailing
                // newlines correctly. If we found junk, throw error.
                throw fq::error::FormatError(
                    fmt::format("Expected '@' at record start. Found '{}'", *ptr));
            }

            const char* line1End = Impl::findEol(ptr, end);
            if (line1End == nullptr) {
                if (impl_->isEofReached) {
                    throw fq::error::FormatError("Incomplete FASTQ record: missing header newline");
                }
                break;
            }

            const char* line2Start = line1End + 1;
            const char* line2End = Impl::findEol(line2Start, end);
            if (line2End == nullptr) {
                if (impl_->isEofReached) {
                    throw fq::error::FormatError("Incomplete FASTQ record: missing sequence line");
                }
                break;
            }

            const char* line3Start = line2End + 1;
            if (line3Start >= end) {
                if (impl_->isEofReached) {
                    throw fq::error::FormatError("Incomplete FASTQ record: missing plus line");
                }
                break;
            }
            if (*line3Start != '+') {
                throw fq::error::FormatError(
                    fmt::format("Expected '+' at line 3. Found '{}'", *line3Start));
            }
            const char* line3End = Impl::findEol(line3Start, end);
            if (line3End == nullptr) {
                if (impl_->isEofReached) {
                    throw fq::error::FormatError("Incomplete FASTQ record: missing plus newline");
                }
                break;
            }

            const char* line4Start = line3End + 1;
            if (line4Start >= end) {
                if (impl_->isEofReached) {
                    throw fq::error::FormatError("Incomplete FASTQ record: missing quality line");
                }
                break;
            }
            const char* line4End = Impl::findEol(line4Start, end);
            if (line4End == nullptr) {
                if (impl_->isEofReached) {
                    line4End = end;
                } else {
                    break;
                }
            }

            FastqRecord rec;

            const auto kIdLen = static_cast<size_t>(line1End - ptr);
            size_t idLen = kIdLen;
            if (idLen > 0 && ptr[idLen - 1] == '\r') {
                --idLen;
            }
            const std::string_view kFullIdLine(ptr + 1, idLen > 0 ? (idLen - 1) : 0);
            const size_t kSpacePos = kFullIdLine.find_first_of(" \t");
            if (kSpacePos != std::string_view::npos) {
                rec.id = kFullIdLine.substr(0, kSpacePos);
                rec.comment = kFullIdLine.substr(kSpacePos + 1);
            } else {
                rec.id = kFullIdLine;
            }

            const auto kSeqLen = static_cast<size_t>(line2End - line2Start);
            size_t seqLen = kSeqLen;
            if (seqLen > 0 && line2Start[seqLen - 1] == '\r') {
                --seqLen;
            }
            rec.seq = std::string_view(line2Start, seqLen);

            const auto kPlusLen = static_cast<size_t>(line3End - line3Start);
            size_t plusLen = kPlusLen;
            if (plusLen > 0 && line3Start[plusLen - 1] == '\r') {
                --plusLen;
            }
            rec.plus = std::string_view(line3Start, plusLen);

            const auto kQualLen = static_cast<size_t>(line4End - line4Start);
            size_t qualLen = kQualLen;
            if (qualLen > 0 && line4Start[qualLen - 1] == '\r') {
                --qualLen;
            }
            rec.qual = std::string_view(line4Start, qualLen);

            if (rec.seq.empty()) {
                throw fq::error::FormatError(fmt::format("Empty sequence for read '{}'", rec.id));
            }
            if (!rec.validateLengths()) {
                throw fq::error::FormatError(
                    fmt::format("Sequence and quality length mismatch for read '{}': {} vs {}",
                                rec.id,
                                rec.seq.size(),
                                rec.qual.size()));
            }

            batch.records().push_back(rec);

            ptr = line4End;
            if (ptr < end && *ptr == '\n') {
                ++ptr;
            }
            lastValidPtr = ptr;
        }

        if (!batch.records().empty()) {
            impl_->learnedParsedBytes += static_cast<double>(lastValidPtr - data);
            impl_->learnedParsedRecords += static_cast<double>(batch.records().size());
            impl_->estimatedRecordBytes = impl_->learnedParsedBytes / impl_->learnedParsedRecords;

            const auto kConsumed = static_cast<size_t>(lastValidPtr - data);
            const auto kTotal = static_cast<size_t>(end - data);
            if (kConsumed < kTotal) {
                // 未消费的残片暂存 reader 侧；resize 复用 remainder 既有容量（clear 不释放）
                const size_t remainderLen = kTotal - kConsumed;
                impl_->remainder.resize(remainderLen);
                std::memcpy(
                    impl_->remainder.data(), batch.buffer().data() + kConsumed, remainderLen);
                batch.buffer().resize(kConsumed);
            }
            return true;
        }

        if (impl_->isEofReached) {
            batch.buffer().clear();
            return false;
        }

        if (impl_->options.maxBufferBytes > 0 &&
            batch.buffer().size() >= impl_->options.maxBufferBytes) {
            throw fq::error::FormatError(
                "FastqReader reached the per-batch buffer capacity without parsing a complete "
                "record (a single read is longer than the buffer); increase --batch-capacity-mb "
                "(library users: ProcessingOptions::batchCapacityBytes / "
                "FastqReaderOptions::maxBufferBytes)");
        }
    }
}

}  // namespace fq::io
