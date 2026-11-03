# SPDX-License-Identifier: AGPL-3.0-only

add_executable(frameworkcpp_memory_bench memory_bench.cpp)
target_link_libraries(frameworkcpp_memory_bench PRIVATE frameworkcpp benchmark::benchmark)
target_compile_features(frameworkcpp_memory_bench PRIVATE cxx_std_20)

add_executable(frameworkcpp_environment_bench environment_bench.cpp)
target_link_libraries(frameworkcpp_environment_bench PRIVATE frameworkcpp benchmark::benchmark)
target_compile_features(frameworkcpp_environment_bench PRIVATE cxx_std_20)

if(FRAMEWORKCPP_ENABLE_REACTOR)
    add_executable(frameworkcpp_reactor_bench reactor_bench.cpp)
    target_link_libraries(
        frameworkcpp_reactor_bench
        PRIVATE frameworkcpp benchmark::benchmark ${FRAMEWORKCPP_URING_LIBRARY}
    )
    target_compile_features(frameworkcpp_reactor_bench PRIVATE cxx_std_20)
endif()
