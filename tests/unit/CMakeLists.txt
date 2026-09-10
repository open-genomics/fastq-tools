# ---------------------------------------------------------------------------
# 单元测试
# 使用 add_fq_test(name label timeout sources...) 注册
# ---------------------------------------------------------------------------

# --- 基础设施 ---
add_fq_test(test_error      "unit" 60  error/test_exceptions.cpp)

# --- 核心功能 ---
add_fq_test(test_io         "unit" 60  io/test_fastq_reader.cpp io/test_writer.cpp)
# 测试直接调用 zlib 生成合法 gzip 数据（损坏/截断场景），显式链接获取头文件与符号
target_link_libraries(test_io PRIVATE ZLIB::ZLIB)
add_fq_test(test_processing "unit" 60  processing/test_pipeline_smoke.cpp processing/test_resolved_runtime_config.cpp processing/test_execution_runtime.cpp processing/test_pipeline_read_statistics.cpp)
add_fq_test(test_cli        "unit" 60  cli/test_filter_plan.cpp cli/test_enum_parser.cpp cli/test_command_registry.cpp)
# fmt 无需在此声明：add_fq_test 已链接 fq_lib，后者 INTERFACE 传播 fmt::fmt。
target_link_libraries(test_cli PRIVATE fq_cli)
add_fq_test(test_predicates "unit" 60  processing/test_predicates.cpp)
add_fq_test(test_mutators   "unit" 60  processing/test_mutators.cpp)
add_fq_test(test_statistics "unit" 60  statistics/test_statistics.cpp statistics/test_statistics_report.cpp)
add_fq_test(test_memory     "unit" 60  memory/test_object_pool.cpp)
# benchmark 结果存储测试：直接把 src/benchmark 两个源文件编进测试目标，
# 保持 BUILD_BENCHMARKS=OFF（默认）时基准工具不参与构建，同时单测始终运行
add_fq_test(test_benchmark  "unit" 60  benchmark/test_result_storage.cpp
    ${CMAKE_SOURCE_DIR}/src/benchmark/result_storage.cpp
    ${CMAKE_SOURCE_DIR}/src/benchmark/data_collector.cpp)
target_link_libraries(test_benchmark PRIVATE nlohmann_json::nlohmann_json)

# --- 聚合目标 ---
add_custom_target(unit_tests
    DEPENDS
        test_error
        test_io
        test_processing
        test_cli
        test_predicates
        test_mutators
        test_statistics
        test_memory
        test_benchmark
)
