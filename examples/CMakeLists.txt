cmake_minimum_required(VERSION 3.16)
project(urma_cpu_to_npu_bench LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(urma_cpu_to_npu_bench urma_cpu_to_npu_bench.cpp)
target_compile_options(urma_cpu_to_npu_bench PRIVATE -O3 -Wall -Wextra -Wpedantic)
target_link_libraries(urma_cpu_to_npu_bench PRIVATE urma pthread)

# Example when the CANN/URMA headers and libraries are not in system paths:
# cmake -S examples -B build/urma \
#   -DCMAKE_CXX_FLAGS="-I/path/to/urma/include" \
#   -DCMAKE_EXE_LINKER_FLAGS="-L/path/to/urma/lib"
