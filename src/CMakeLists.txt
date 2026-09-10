# Add core components
add_subdirectory(error)

add_subdirectory(io)

# 批次级统计核只依赖 I/O，必须在 processing 之前定义：
# Pipeline 可选 reduce 链接它，statistics runner 也链接它，避免循环依赖。
add_library(fq_stat_core STATIC
    statistics/fq_statistic_result.cpp
    statistics/fq_statistic_worker.cpp
)
target_include_directories(fq_stat_core
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/statistics
)
target_link_libraries(fq_stat_core
    PUBLIC
        fq_modern_io
    PRIVATE
        fq_error
)
target_compile_features(fq_stat_core PUBLIC cxx_std_23)

add_subdirectory(processing)
add_subdirectory(statistics)
add_subdirectory(cli)

# benchmark 数据收集模块仅在需要时编译
if(BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()

# 聚合库：将所有组件统一为一个 INTERFACE target，供 CLI 层链接
# 显式列出全部组件（含已被传递的底层库 fq_error/fq_modern_io），保证聚合依赖可见性
add_library(fq_runtime INTERFACE)
set_target_properties(fq_runtime PROPERTIES EXPORT_NAME FastQTools)

target_link_libraries(fq_runtime
    INTERFACE
        fq_error
        fq_modern_io
        fq_stat_core
        fq_processing
        fq_statistics
        # fmt 与上面那行 include/ 是同一件事的两半：include/fqtools/logging.h
        # 是公共头，其内联函数直接使用 fmt。本目标既然向下游传播「能看见
        # logging.h」，就必须同时传播「能编译 logging.h」所需的 fmt。
        #
        # 之所以挂在这里而不是让各目标自行声明：依赖以 PRIVATE 链接时不向下游
        # 传播，于是「谁 include 了 logging.h」与「谁链接了 fmt」变成两份需要
        # 人工保持同步的清单——实测已漏过两处（fq_cli、tests/unit/test_cli），
        # 且因开发机装有系统 fmt 而只在干净环境暴露。挂到本目标后，这两个集合
        # 由构造保证一致，不再有遗漏面。
        #
        # 导出侧无需额外处理：cmake/FastQToolsConfig.cmake.in 早已声明
        # find_dependency(fmt CONFIG) 并处理了 Conan 的 fmt-header-only 别名。
        fmt::fmt
)

target_include_directories(fq_runtime
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Internal compatibility name; never installed or exported.
add_library(fq_lib ALIAS fq_runtime)
