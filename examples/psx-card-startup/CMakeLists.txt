cmake_minimum_required(VERSION 3.16)

project(psx_card_startup LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(psx_card_startup src/psx_card_startup.cpp)
target_include_directories(psx_card_startup PUBLIC include)

add_executable(psx_card_startup_tests tests/card_startup_test.cpp)
target_link_libraries(psx_card_startup_tests PRIVATE psx_card_startup)

function(psx_card_warnings target)
  target_compile_options(
    ${target}
    PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wpedantic -Werror>
      $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive->)
endfunction()

psx_card_warnings(psx_card_startup)
psx_card_warnings(psx_card_startup_tests)

enable_testing()
add_test(NAME psx_card_startup_tests COMMAND psx_card_startup_tests)
