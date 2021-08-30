# CMakeList.txt : CMake project for Dragon, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.8)

# Add source to this project's executable.
add_executable (Dragon "src/Dragon.c"  "src/common.h" "src/chunk.h" "src/chunk.c" "src/memory.h" "src/memory.c" "src/debug.h" "src/debug.c" "src/value.h" "src/value.c" "src/vm.h" "src/vm.c" "src/compiler.h" "src/compiler.c" "src/scanner.h" "src/scanner.c" "src/object.c" "src/object.h" "src/table.h" "src/table.c" "src/leb128.c" "src/leb128.h" "src/natives.c" "src/natives.h" "src/exception.h" "src/exception.c")
