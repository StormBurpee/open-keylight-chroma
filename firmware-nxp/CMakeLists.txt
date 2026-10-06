cmake_minimum_required(VERSION 3.20)

# Standalone host software tests; this is not an ARM deployment target.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(open_keylight_nxp_tests C)
  enable_testing()
endif()

add_library(nxp_application STATIC src/nxp_app.c src/nxp_board.c)
target_include_directories(nxp_application PUBLIC include)
set_property(TARGET nxp_application PROPERTY C_STANDARD 99)

add_executable(test_nxp_protocol tests/test_nxp.c
  ../firmware/components/keylight_nxp/okl_nxp.c)
target_include_directories(test_nxp_protocol PRIVATE ../firmware/components/keylight_nxp/include)
target_link_libraries(test_nxp_protocol PRIVATE nxp_application)

add_executable(test_nxp_board tests/test_board.c)
target_link_libraries(test_nxp_board PRIVATE nxp_application)

foreach(target nxp_application test_nxp_protocol test_nxp_board)
  set_property(TARGET ${target} PROPERTY C_STANDARD 99)
  set_property(TARGET ${target} PROPERTY C_STANDARD_REQUIRED ON)
  if(NOT MSVC)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wpedantic)
  endif()
endforeach()
add_test(NAME nxp_protocol COMMAND test_nxp_protocol)
add_test(NAME nxp_board COMMAND test_nxp_board)
