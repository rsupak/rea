cmake_minimum_required(VERSION 3.16)

project(psx_cd_loc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(psx_cd_loc src/psx_cd_loc.cpp)
target_include_directories(psx_cd_loc PUBLIC include)

add_executable(psx_cd_loc_tests tests/cd_loc_test.cpp)
target_link_libraries(psx_cd_loc_tests PRIVATE psx_cd_loc)

function(psx_cd_loc_warnings target)
  target_compile_options(
    ${target}
    PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>
      $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive->)
endfunction()

psx_cd_loc_warnings(psx_cd_loc)
psx_cd_loc_warnings(psx_cd_loc_tests)

enable_testing()
add_test(NAME psx_cd_loc_tests COMMAND psx_cd_loc_tests)
