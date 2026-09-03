#include "processing/execution_runtime.h"

#include "fqtools/error/error.h"
#include "fqtools/io/fastq_reader.h"
#include "fqtools/io/fastq_writer.h"

#include <stdexcept>
#include <utility>

#include "io/path_alias.h"
#include "processing/resolved_runtime_config.h"

namespace {

auto makeReaderOptions(const fq::processing::ResolvedRuntimeConfig& config)
    -> fq::io::FastqReaderOptions {
    fq::io::FastqReaderOptions options;
    options.readChunkBytes = config.readChunkBytes;
    options.zlibBufferBytes = config.zlibBufferBytes;
    options.maxBufferBytes = config.batchCapacityBytes;
    return options;
}

auto makeWriterOptions(const fq::processing::ResolvedRuntimeConfig& config)
    -> fq::io::FastqWriterOptions {
    fq::io::FastqWriterOptions options;
    options.zlibBufferBytes = config.zlibBufferBytes;
    options.outputBufferBytes = config.writerBufferBytes;
    return options;
}

}  // namespace

namespace fq::processing {

namespace {

auto selectBackend(ExecutionBackendPreference preference, const ResolvedRuntimeConfig& config)
    -> std::unique_ptr<ExecutionBackend> {
    switch (preference) {
        case ExecutionBackendPreference::Automatic:
            return config.executionMode == ExecutionMode::Parallel
                ? createOneTbbExecutionBackend()
                : createSequentialExecutionBackend();
        case ExecutionBackendPreference::Sequential:
            return createSequentialExecutionBackend();
        case ExecutionBackendPreference::OneTbb:
            return createOneTbbExecutionBackend();
    }

    throw std::invalid_argument("Unknown execution backend preference");
}

}  // namespace

struct ExecutionRuntime::Impl {
    std::unique_ptr<fq::io::IReader> customReader;
    std::shared_ptr<fq::io::IWriter> customWriter;
    bool customReaderConfigured = false;
};

ExecutionRuntime::ExecutionRuntime() : impl_(std::make_unique<Impl>()) {}

ExecutionRuntime::ExecutionRuntime(std::unique_ptr<fq::io::IReader> customReader,
                                   std::shared_ptr<fq::io::IWriter> customWriter)
    : impl_(std::make_unique<Impl>()) {
    impl_->customReaderConfigured = customReader != nullptr;
    impl_->customReader = std::move(customReader);
    impl_->customWriter = std::move(customWriter);
}

ExecutionRuntime::~ExecutionRuntime() = default;
ExecutionRuntime::ExecutionRuntime(ExecutionRuntime&&) noexcept = default;
auto ExecutionRuntime::operator=(ExecutionRuntime&&) noexcept -> ExecutionRuntime& = default;

auto ExecutionRuntime::executeErased(const ExecutionRuntimeRequest& request,
                                     ExecutionOperation& operation) -> ErasedExecutionOutcome {
    request.options.validate();
    if (request.outputPath && fq::io::pathsAlias(request.inputPath, *request.outputPath)) {
        throw fq::error::ConfigurationError("input and output paths must be different");
    }
    if (impl_->customReaderConfigured && !impl_->customReader) {
        throw std::invalid_argument(
            "ExecutionRuntime: custom reader must be reset before rerunning");
    }

    const auto config = resolveRuntimeConfig(
        request.options, impl_->customReader != nullptr, impl_->customWriter != nullptr);

    std::shared_ptr<fq::io::IReader> reader;
    if (impl_->customReader) {
        reader = std::shared_ptr<fq::io::IReader>(std::move(impl_->customReader));
    } else {
        // FastqReader/FastqWriter 构造失败时直接抛 IOError，无需 isOpen() 复查
        reader =
            std::make_shared<fq::io::FastqReader>(request.inputPath, makeReaderOptions(config));
    }

    auto writer = impl_->customWriter;
    if (!writer && request.outputPath) {
        writer =
            std::make_shared<fq::io::FastqWriter>(*request.outputPath, makeWriterOptions(config));
    }

    auto backend = selectBackend(request.backend, config);
    return backend->execute(
        {std::move(reader), std::move(writer), config, request.options.batchSize}, operation);
}

}  // namespace fq::processing
