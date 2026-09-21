# FastQTools 基准测试

cmake_minimum_required(VERSION 3.20)

# 查找 Google Benchmark
find_package(benchmark CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# 通用链接库：Google Benchmark + 项目主库 + 内部 benchmark 支撑库
# nlohmann_json 由 fq_benchmark PUBLIC 传播，无需重复列出
set(BENCHMARK_COMMON_LIBS
    benchmark::benchmark
    fq_lib
    fq_benchmark
)

# 基准测试工具函数（使用自定义 main 注入系统元数据）
function(add_benchmark benchmark_name source_files)
    add_executable(${benchmark_name} ${source_files} benchmark_main.cpp)
    
    target_link_libraries(${benchmark_name} PRIVATE
        ${BENCHMARK_COMMON_LIBS}
    )
    
    target_include_directories(${benchmark_name} PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    
    # 设置基准测试属性
    set_target_properties(${benchmark_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
endfunction()

# FastQ 读写基准测试（基于新 IO）
add_benchmark(benchmark_fastq_io
    fastq_io_benchmark.cpp
)

# Filter 命令基准测试
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/filter_benchmark.cpp")
    add_benchmark(benchmark_filter
        filter_benchmark.cpp
    )
    target_include_directories(benchmark_filter PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()

# Stat 命令基准测试
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/stat_benchmark.cpp")
    add_benchmark(benchmark_stat
        stat_benchmark.cpp
    )
    target_include_directories(benchmark_stat PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()

# Pipeline 流水线基准测试
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/pipeline_benchmark.cpp")
    add_benchmark(benchmark_pipeline
        pipeline_benchmark.cpp
    )
    target_include_directories(benchmark_pipeline PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()

# 并发扩展基准测试：真实 Pipeline 的线程数/批大小扫描。
# 这是探索性矩阵，不加入默认 run_benchmarks，避免改变固定 v4 基线的耗时和口径。
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/concurrency_benchmark.cpp")
    add_benchmark(benchmark_concurrency
        concurrency_benchmark.cpp
    )
    target_include_directories(benchmark_concurrency PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()

# ObjectPool 对象池基准测试
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/object_pool_benchmark.cpp")
    add_benchmark(benchmark_object_pool
        object_pool_benchmark.cpp
    )
endif()

# 端到端性能基准测试
if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/performance_benchmark.cpp")
    add_executable(performance_benchmark
        performance_benchmark.cpp
    )

    target_link_libraries(performance_benchmark PRIVATE
        fq_cli
        fmt::fmt
    )

    target_include_directories(performance_benchmark PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
endif()

# 收集所有基准测试目标
set(ALL_BENCHMARK_TARGETS benchmark_fastq_io)

if (TARGET benchmark_filter)
    list(APPEND ALL_BENCHMARK_TARGETS benchmark_filter)
endif()

if (TARGET benchmark_stat)
    list(APPEND ALL_BENCHMARK_TARGETS benchmark_stat)
endif()

if (TARGET benchmark_pipeline)
    list(APPEND ALL_BENCHMARK_TARGETS benchmark_pipeline)
endif()

if (TARGET benchmark_object_pool)
    list(APPEND ALL_BENCHMARK_TARGETS benchmark_object_pool)
endif()

# 创建基准测试组
add_custom_target(benchmarks
    DEPENDS ${ALL_BENCHMARK_TARGETS}
    COMMENT "Building all benchmarks"
)

# 定义输出目录
set(BENCHMARK_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/docs/performance/benchmark-reports/results")

# 运行所有基准测试的目标（固定 1M×150 bp 数据，5 次重复并生成 median/CV）
set(BENCHMARK_RUNNER "${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_benchmarks.py")

add_custom_target(run_benchmarks
    COMMAND ${Python3_EXECUTABLE} ${BENCHMARK_RUNNER}
        --build-dir ${CMAKE_BINARY_DIR}
        --output-dir ${BENCHMARK_OUTPUT_DIR}
        --repetitions 5
    DEPENDS benchmarks
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running all benchmarks with JSON output"
)

# 完整基准测试目标（包含所有测试和报告生成）
add_custom_target(benchmark_full
    COMMAND ${Python3_EXECUTABLE} ${BENCHMARK_RUNNER}
        --build-dir ${CMAKE_BINARY_DIR}
        --output-dir ${BENCHMARK_OUTPUT_DIR}
        --repetitions 5
    DEPENDS benchmarks
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running full benchmark suite with multiple repetitions"
)

# CI 模式基准测试目标
add_custom_target(benchmark_ci
    COMMAND ${Python3_EXECUTABLE} ${BENCHMARK_RUNNER}
        --build-dir ${CMAKE_BINARY_DIR}
        --output-dir ${BENCHMARK_OUTPUT_DIR}/ci
        --repetitions 5
    DEPENDS benchmarks
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running benchmarks in CI mode"
)
