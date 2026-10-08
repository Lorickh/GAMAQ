cmake_minimum_required(VERSION 3.16)
project(urma_cpu_to_npu_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_URMA_BENCH "Build benchmark requiring the target URMA SDK" ON)
if(BUILD_URMA_BENCH)
    add_executable(urma_cpu_to_npu_bench urma_cpu_to_npu_bench.cpp)
    target_compile_options(urma_cpu_to_npu_bench PRIVATE -O3 -Wall -Wextra -Wpedantic)
    target_link_libraries(urma_cpu_to_npu_bench PRIVATE urma pthread)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(transfer_layout_test tests/transfer_layout_test.cpp)
    target_include_directories(transfer_layout_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(transfer_layout_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME transfer_layout COMMAND transfer_layout_test)

    add_executable(completion_tracker_test tests/completion_tracker_test.cpp)
    target_include_directories(completion_tracker_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(completion_tracker_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME completion_tracker COMMAND completion_tracker_test)

    add_executable(window_scheduler_test tests/window_scheduler_test.cpp)
    target_include_directories(window_scheduler_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(window_scheduler_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME window_scheduler COMMAND window_scheduler_test)

    add_executable(kv_put_batch_test tests/kv_put_batch_test.cpp)
    target_include_directories(kv_put_batch_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(kv_put_batch_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME kv_put_batch COMMAND kv_put_batch_test)

    add_executable(registered_region_test tests/registered_region_test.cpp)
    target_include_directories(registered_region_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(registered_region_test PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME registered_region COMMAND registered_region_test)

    if(BUILD_URMA_BENCH)
        # Uses official URMA types but stubs post/poll, so no device is needed.
        add_executable(urma_kv_backend_test tests/urma_kv_backend_test.cpp)
        target_include_directories(urma_kv_backend_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(urma_kv_backend_test PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME urma_kv_backend COMMAND urma_kv_backend_test)

        # Exercises lifecycle and session-bound KV put with stubbed provider calls.
        add_executable(urma_session_test tests/urma_session_test.cpp)
        target_include_directories(urma_session_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_options(urma_session_test PRIVATE -Wall -Wextra -Wpedantic)
        add_test(NAME urma_session COMMAND urma_session_test)
    endif()
endif()

# Example when the CANN/URMA headers and libraries are not in system paths:
# cmake -S examples -B build/urma \
#   -DCMAKE_CXX_FLAGS="-I/path/to/urma/include" \
#   -DCMAKE_EXE_LINKER_FLAGS="-L/path/to/urma/lib"
