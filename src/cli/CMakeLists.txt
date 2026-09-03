add_library(fq_cli STATIC
    filter_plan.cpp
    filter_plan.h
    commands/filter_command.cpp
    commands/filter_command.h
    commands/stat_command.cpp
    commands/stat_command.h
    commands/command_interface.h
    command_registry.cpp
    command_registry.h
    common_options.cpp
    common_options.h
    enum_parser.cpp
    enum_parser.h
)

# cxxopts 为 PUBLIC：公共头 filter_plan.h 直接 include <cxxopts.hpp>
# fq_runtime 为 PRIVATE：仅 .cpp 使用，不暴露给消费者
target_link_libraries(fq_cli
    PUBLIC
        cxxopts::cxxopts
    PRIVATE
        fq_runtime
)

target_include_directories(fq_cli
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        # 跨模块内部头（io/path_alias.h），与 fq_processing/fq_statistics 同一模式
        ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(fq_cli PUBLIC cxx_std_23)

# --- 可执行入口 ---
# main.cpp 与 fq_cli 同目录，归属此处而非根 CMakeLists.txt
add_executable(FastQTools main.cpp)

# 产物固定输出到构建根，避免下沉后路径变为 build/src/cli/FastQTools
# 下游脚本/tests 默认期望 ${CMAKE_BINARY_DIR}/FastQTools
set_target_properties(FastQTools PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# cxxopts 由 fq_cli PUBLIC 传播；fmt 仍需显式（main.cpp 经 logging.h 内联函数使用）
target_link_libraries(FastQTools
    PRIVATE
        fq_cli
        fmt::fmt
)

target_include_directories(FastQTools PRIVATE ${CMAKE_SOURCE_DIR}/include)
