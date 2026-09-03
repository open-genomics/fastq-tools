#include "fqtools/io/fastq_writer.h"

#include "fqtools/error/error.h"
#include "fqtools/logging.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>

#include <sys/stat.h>

namespace fq::io {

namespace {

struct TemporaryFile {
    std::filesystem::path path;
    int fd = -1;
};

// 创建临时文件并返回持有其 fd 的结果：后续 gz/plain 写入直接复用该 fd，
// 消除"O_EXCL 创建后关闭、再以普通 open 重开"之间的 TOCTOU 窗口。
auto makeTemporaryFile(const std::filesystem::path& target) -> TemporaryFile {
    static std::atomic<std::uint64_t> counter{0};
    const auto parent =
        target.parent_path().empty() ? std::filesystem::path(".") : target.parent_path();
    const auto filename = target.filename().string();
    const auto processId = static_cast<unsigned long long>(::getpid());
    for (;;) {
        const auto suffix = ".tmp-" + std::to_string(processId) + "-" +
            std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
        const auto candidate = parent / (filename + suffix);
        // 0666 交由 umask 过滤（惯例 0644）：rename 发布保留该权限，
        // 固定 0600 会无视调用方 umask，产出比预期更严格的权限
        const int descriptor = ::open(candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (descriptor >= 0) {
            return {candidate, descriptor};
        }
        if (errno != EEXIST) {
            throw fq::error::IOError(candidate.string(), errno);
        }
    }
}

}  // namespace

static auto endsWithGzSuffix(const std::string& path) -> bool {
    constexpr const char* kGz = ".gz";
    if (path.size() < 3) {
        return false;
    }
    return path.compare(path.size() - 3, 3, kGz) == 0;
}

// 目标是否为特殊文件（字符/块设备、FIFO、socket）：
// 这些目标不支持"同目录临时文件 + rename"的原子发布协议——
// 在 /dev 下创建临时文件会被拒绝（Permission denied），
// 且对设备做 rename 覆盖也无意义。此类目标直接写入（与 stdout 语义一致）。
// 目标不存在或 stat 失败时按普通文件处理（走原子路径，open 阶段报错）。
auto isSpecialFileTarget(const std::string& path) -> bool {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        return false;
    }
    return S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode) || S_ISFIFO(st.st_mode) ||
        S_ISSOCK(st.st_mode);
}

struct FastqWriter::Impl {
    int fd = -1;
    gzFile gzfile = nullptr;
    bool ownsFd = true;
    bool streamingStdout = false;
    bool directWrite = false;  // 特殊文件目标（/dev/null 等）：无临时文件 + rename
    std::string path;
    std::filesystem::path temporaryPath;
    FastqWriterOptions options{};
    FastqWriterCompressionMode compression = FastqWriterCompressionMode::Auto;
    std::vector<char> buffer;

    // writer 只在执行后端的串行输出阶段被访问（IWriter 契约），无需原子计数
    std::uint64_t totalUncompressedBytes = 0;
    bool finished = false;

    // 记录已刷写到磁盘的文件偏移，用于 posix_fadvise DONTNEED
    off_t flushedOffset = 0;

    // gzdopen 会接管临时文件 fd 并在 gzclose 时关闭；dup 一份仅用于发布前 fsync，
    // 使 rename 前的数据页真正落盘（原子发布在掉电场景下的持久性保证）
    int syncFd = -1;

    explicit Impl(std::string p, const FastqWriterOptions& opt) : path(std::move(p)), options(opt) {
        if (path == "-") {
            openStdout();
            return;
        }

        if (isSpecialFileTarget(path)) {
            openDirect();
            return;
        }

        auto temp = makeTemporaryFile(path);
        temporaryPath = temp.path;

        try {
            if (options.compression == FastqWriterCompressionMode::Auto) {
                compression = endsWithGzSuffix(path) ? FastqWriterCompressionMode::Gzip
                                                     : FastqWriterCompressionMode::None;
            } else {
                compression = options.compression;
            }

            if (compression == FastqWriterCompressionMode::Gzip) {
                if (options.compressionLevel < 1 || options.compressionLevel > 9) {
                    throw fq::error::ConfigurationError(
                        "gzip compression level must be between 1 and 9");
                }
                // 使用 zlib gz API 写入 gzip 文件，默认压缩级别 6。
                // gzdopen 接管临时文件 fd（gzclose 时一并关闭），不再重开文件。
                const std::string mode = "wb" + std::to_string(options.compressionLevel);
                // dup 供发布前 fsync（fd 所有权随后移交 gzFile，gzclose 会关闭原 fd）
                syncFd = ::dup(temp.fd);
                gzfile = gzdopen(temp.fd, mode.c_str());
                if (!gzfile) {
                    throw fq::error::IOError(temporaryPath.string(), errno);
                }
                temp.fd = -1;  // fd 所有权移交给 gzFile
                gzbuffer(gzfile, static_cast<unsigned>(options.outputBufferBytes));
            } else {
                fd = temp.fd;
                temp.fd = -1;
            }

            buffer.reserve(options.outputBufferBytes);
        } catch (...) {
            if (temp.fd >= 0) {
                ::close(temp.fd);
            }
            if (gzfile) {
                gzclose(gzfile);
                gzfile = nullptr;
            }
            std::error_code error;
            std::filesystem::remove(temporaryPath, error);
            throw;
        }
    }

    void openStdout() {
        streamingStdout = true;
        ownsFd = false;
        fd = STDOUT_FILENO;
        compression = FastqWriterCompressionMode::None;
        buffer.reserve(options.outputBufferBytes);
    }

    // 特殊文件目标（字符/块设备、FIFO、socket）：绕过临时文件 + rename，
    // 直接以目标 fd 顺序写入；finish() 只 flush/关闭，不做发布。
    void openDirect() {
        directWrite = true;
        ownsFd = true;
        fd = ::open(path.c_str(), O_WRONLY | O_CREAT, 0666);
        if (fd < 0) {
            throw fq::error::IOError(path, errno);
        }
        if (options.compression == FastqWriterCompressionMode::Auto) {
            compression = endsWithGzSuffix(path) ? FastqWriterCompressionMode::Gzip
                                                 : FastqWriterCompressionMode::None;
        } else {
            compression = options.compression;
        }
        if (compression == FastqWriterCompressionMode::Gzip) {
            if (options.compressionLevel < 1 || options.compressionLevel > 9) {
                ::close(fd);
                fd = -1;
                throw fq::error::ConfigurationError(
                    "gzip compression level must be between 1 and 9");
            }
            const std::string mode = "wb" + std::to_string(options.compressionLevel);
            gzfile = gzdopen(fd, mode.c_str());
            if (!gzfile) {
                const int savedErrno = errno;
                ::close(fd);
                fd = -1;
                throw fq::error::IOError(path, savedErrno);
            }
            fd = -1;  // fd 所有权移交给 gzFile
            gzbuffer(gzfile, static_cast<unsigned>(options.outputBufferBytes));
        }
        buffer.reserve(options.outputBufferBytes);
    }

    ~Impl() {
        // 析构只做兜底清理，绝不发布输出：异常展开到达这里时内容必然不完整，
        // 若调用 finish() 会把截断结果 rename 成目标文件，被误认为完整产物。
        // 发布只能经由显式 finish()（见 IWriter::finish 契约）。
        if (!finished) {
            cleanupTemporaryFile();
        }
    }

    void cleanupTemporaryFile() noexcept {
        if (ownsFd && fd >= 0) {
            ::close(fd);
            fd = -1;
        }
        if (gzfile) {
            gzclose(gzfile);
            gzfile = nullptr;
        }
        if (syncFd >= 0) {
            ::close(syncFd);
            syncFd = -1;
        }
        if (!temporaryPath.empty()) {
            std::error_code error;
            std::filesystem::remove(temporaryPath, error);
        }
    }

    void finish() {
        if (finished) {
            return;
        }

        try {
            flush();

            if (streamingStdout) {
                finished = true;
                return;
            }

            if (directWrite) {
                // 特殊文件目标：无 rename 发布。设备/FIFO 无 page cache 语义，
                // 跳过 fsync；正确关闭即可（gzclose 会关闭其接管的 fd）。
                if (gzfile) {
                    const int closeResult = gzclose(gzfile);
                    gzfile = nullptr;
                    if (closeResult != Z_OK) {
                        throw fq::error::FastQException(fq::error::ErrorCategory::IO,
                                                        fq::error::ErrorSeverity::Critical,
                                                        "gzip close failed");
                    }
                } else if (ownsFd && fd >= 0) {
                    if (::close(fd) != 0) {
                        const int error = errno;
                        fd = -1;
                        throw fq::error::IOError(path, error);
                    }
                    fd = -1;
                }
                finished = true;
                return;
            }

            if (ownsFd && fd >= 0) {
                // 数据落盘后再关闭：rename 发布前确保内容已到存储介质
                // （best-effort：fsync 失败不阻断发布，兼容 tmpfs 等无持久语义的挂载点）
                if (::fsync(fd) != 0) {
                    fq::logging::warn("fsync failed for '{}': {}", path, std::strerror(errno));
                }
                if (::close(fd) != 0) {
                    const int error = errno;
                    fd = -1;
                    throw fq::error::IOError(path, error);
                }
                fd = -1;
            }

            if (gzfile) {
                const int closeResult = gzclose(gzfile);
                gzfile = nullptr;
                if (closeResult != Z_OK) {
                    throw fq::error::FastQException(fq::error::ErrorCategory::IO,
                                                    fq::error::ErrorSeverity::Critical,
                                                    "gzip close failed");
                }
                if (syncFd >= 0) {
                    if (::fsync(syncFd) != 0) {
                        fq::logging::warn("fsync failed for '{}': {}", path, std::strerror(errno));
                    }
                    ::close(syncFd);
                    syncFd = -1;
                }
            }

            std::error_code error;
            std::filesystem::rename(temporaryPath, path, error);
            if (error) {
                throw fq::error::IOError(path, error.value());
            }
            // rename 本身不保证持久：fsync 父目录使改名记录落盘
            // （best-effort，目录打开失败或 fsync 失败不阻断发布）
            const auto parent = std::filesystem::path(path).parent_path().empty()
                ? std::filesystem::path(".")
                : std::filesystem::path(path).parent_path();
            const int dirFd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
            if (dirFd >= 0) {
                ::fsync(dirFd);
                ::close(dirFd);
            }
            finished = true;
        } catch (...) {
            cleanupTemporaryFile();
            throw;
        }
    }

    void flush() {
        if (buffer.empty()) {
            return;
        }

        if (compression == FastqWriterCompressionMode::Gzip) {
            // gzwrite 接受 unsigned int 参数：分块写出避免大缓冲静默截断。
            // 返回写入的未压缩字节数（0 表示错误）
            constexpr size_t kMaxGzChunk =
                static_cast<size_t>(std::numeric_limits<unsigned>::max()) / 2;
            const char* outPtr = buffer.data();
            size_t remaining = buffer.size();
            while (remaining > 0) {
                const auto toWrite = static_cast<unsigned>(std::min(remaining, kMaxGzChunk));
                const int written = gzwrite(gzfile, outPtr, toWrite);
                if (written <= 0 || static_cast<unsigned>(written) != toWrite) {
                    int err = 0;
                    const char* msg = gzerror(gzfile, &err);
                    throw fq::error::FastQException(fq::error::ErrorCategory::IO,
                                                    fq::error::ErrorSeverity::Critical,
                                                    msg ? msg : "gzwrite failed");
                }
                outPtr += written;
                remaining -= static_cast<size_t>(written);
            }
        } else {
            const char* outPtr = buffer.data();
            size_t outSize = buffer.size();
            size_t totalWritten = 0;
            while (totalWritten < outSize) {
                const ssize_t written = ::write(fd, outPtr + totalWritten, outSize - totalWritten);
                if (written < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    throw fq::error::IOError(path, errno);
                }
                if (written == 0) {
                    throw fq::error::IOError(path, 0);
                }
                totalWritten += static_cast<size_t>(written);
            }

#ifdef __linux__
            if (!streamingStdout && !directWrite) {
                // 通知内核释放已写出的 page cache，减少大文件写出时的内存压力
                ::posix_fadvise(
                    fd, flushedOffset, static_cast<off_t>(outSize), POSIX_FADV_DONTNEED);
                flushedOffset += static_cast<off_t>(outSize);
            }
#endif
        }

        buffer.clear();
    }

    /// 批量追加：先计算总大小，一次 resize，然后用 memcpy 拼接
    void appendRecord(const FastqRecord& rec) {
        const std::string_view plusLine = rec.plus.empty() ? std::string_view("+") : rec.plus;
        size_t needed = 1 + rec.id.size() + 1 +  // @ + ID + \n
            rec.seq.size() + 1 +                 // Seq + \n
            plusLine.size() + 1 +                // Plus + \n
            rec.qual.size() + 1;                 // Qual + \n

        if (!rec.comment.empty()) {
            needed += 1 + rec.comment.size();  // Space + Comment
        }

        // Flush if buffer full
        if (buffer.size() + needed > buffer.capacity()) {
            flush();

            if (needed > buffer.capacity()) {
                size_t newCap = std::max(buffer.capacity() * 2, needed + 4096);
                buffer.reserve(newCap);
            }
        }

        // 一次 resize + memcpy 批量拼接，避免逐字符 push_back/insert 开销；
        // 空 string_view 的 data() 可能为 nullptr，memcpy(nullptr, 0) 标准上是 UB，
        // 统一经 appendView 判空后拷贝
        const size_t oldSize = buffer.size();
        buffer.resize(oldSize + needed);
        char* dst = buffer.data() + oldSize;

        const auto appendView = [&dst](std::string_view value) {
            if (!value.empty()) {
                std::memcpy(dst, value.data(), value.size());
                dst += value.size();
            }
        };

        *dst++ = '@';
        appendView(rec.id);

        if (!rec.comment.empty()) {
            *dst++ = ' ';
            appendView(rec.comment);
        }
        *dst++ = '\n';

        appendView(rec.seq);
        *dst++ = '\n';

        appendView(plusLine);
        *dst++ = '\n';

        appendView(rec.qual);
        *dst++ = '\n';

        // 记录确实进入缓冲区后才计数：flush 抛异常时不把未接受的字节算进提交量
        totalUncompressedBytes += needed;
    }
};

FastqWriter::FastqWriter(const std::string& path) : FastqWriter(path, FastqWriterOptions{}) {}

FastqWriter::FastqWriter(const std::string& path, const FastqWriterOptions& options)
    : impl_(std::make_unique<Impl>(path, options)) {
    // Constructor logic verified in Impl
}

FastqWriter::~FastqWriter() = default;

FastqWriter::FastqWriter(FastqWriter&&) noexcept = default;
FastqWriter& FastqWriter::operator=(FastqWriter&&) noexcept = default;

bool FastqWriter::isOpen() const {
    return impl_ && (impl_->fd >= 0 || impl_->gzfile != nullptr);
}

auto FastqWriter::write(const FastqBatch& batch) -> std::uint64_t {
    if (!impl_ || impl_->finished) {
        throw fq::error::ConfigurationError("FastqWriter cannot write after finish");
    }
    const auto before = totalUncompressedBytes();
    for (const auto& rec : batch) {
        impl_->appendRecord(rec);
    }
    return totalUncompressedBytes() - before;
}

void FastqWriter::write(const FastqRecord& record) {
    if (!impl_ || impl_->finished) {
        throw fq::error::ConfigurationError("FastqWriter cannot write after finish");
    }
    impl_->appendRecord(record);
}

void FastqWriter::finish() {
    if (!impl_) {
        throw fq::error::ConfigurationError("FastqWriter is not initialized");
    }
    impl_->finish();
}

auto FastqWriter::totalUncompressedBytes() const -> std::uint64_t {
    return impl_ ? impl_->totalUncompressedBytes : 0;
}

}  // namespace fq::io
