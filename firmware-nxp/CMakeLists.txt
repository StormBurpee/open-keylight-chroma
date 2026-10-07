cmake_minimum_required(VERSION 3.20)

# Standalone host software tests; this is not an ARM deployment target.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(open_keylight_nxp_tests C)
  enable_testing()
endif()

add_library(nxp_application STATIC src/nxp_app.c src/nxp_board.c src/nxp_pwm_off_trial.c)
target_include_directories(nxp_application PUBLIC include)
set_property(TARGET nxp_application PROPERTY C_STANDARD 99)

add_executable(test_nxp_protocol tests/test_nxp.c
  ../firmware/components/keylight_nxp/okl_nxp.c ../firmware/main/output_policy.c
  ../firmware/components/keylight_core/keylight_core.c)
target_include_directories(test_nxp_protocol PRIVATE ../firmware/components/keylight_nxp/include
  ../firmware/main ../firmware/components/keylight_core/include)
target_link_libraries(test_nxp_protocol PRIVATE nxp_application)
if(NOT MSVC)
  target_link_libraries(test_nxp_protocol PRIVATE m)
endif()

add_executable(test_nxp_board tests/test_board.c)
target_link_libraries(test_nxp_board PRIVATE nxp_application)
add_executable(test_nxp_pwm tests/test_pwm.c)
target_link_libraries(test_nxp_pwm PRIVATE nxp_application)
add_executable(test_nxp_pwm_off tests/test_pwm_off.c)
target_link_libraries(test_nxp_pwm_off PRIVATE nxp_application)

foreach(target nxp_application test_nxp_protocol test_nxp_board test_nxp_pwm test_nxp_pwm_off)
  set_property(TARGET ${target} PROPERTY C_STANDARD 99)
  set_property(TARGET ${target} PROPERTY C_STANDARD_REQUIRED ON)
  if(NOT MSVC)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Wpedantic)
  endif()
endforeach()
add_test(NAME nxp_protocol COMMAND test_nxp_protocol)
add_test(NAME nxp_board COMMAND test_nxp_board)
add_test(NAME nxp_pwm COMMAND test_nxp_pwm)
add_test(NAME nxp_pwm_off COMMAND test_nxp_pwm_off)

# Keep LOW1 separate from the OFF1/default state layout and binary profile.
add_executable(test_nxp_pwm_low tests/test_pwm_low.c src/nxp_app.c
  src/nxp_board.c src/nxp_pwm_low_trial.c)
target_compile_definitions(test_nxp_pwm_low PRIVATE NXP_PWM_LOW_TRIAL=1)
target_include_directories(test_nxp_pwm_low PRIVATE include ../firmware/main
  ../firmware/components/keylight_nxp/include)
set_property(TARGET test_nxp_pwm_low PROPERTY C_STANDARD 99)
set_property(TARGET test_nxp_pwm_low PROPERTY C_STANDARD_REQUIRED ON)
if(NOT MSVC)
  target_compile_options(test_nxp_pwm_low PRIVATE -Wall -Wextra -Werror -Wpedantic)
endif()
add_test(NAME nxp_pwm_low COMMAND test_nxp_pwm_low)

# Reference lighting has independent fail-dark/update-order tests; its macro
# cannot alter either frozen diagnostic binary.
foreach(name pwm_production power_handoff)
  add_executable(test_nxp_${name} tests/test_${name}.c src/nxp_app.c src/nxp_board.c)
  target_compile_definitions(test_nxp_${name} PRIVATE NXP_PRODUCTION_LIGHTING=1)
  target_include_directories(test_nxp_${name} PRIVATE include)
  set_property(TARGET test_nxp_${name} PROPERTY C_STANDARD 99)
  set_property(TARGET test_nxp_${name} PROPERTY C_STANDARD_REQUIRED ON)
  if(NOT MSVC)
    target_compile_options(test_nxp_${name} PRIVATE -Wall -Wextra -Werror -Wpedantic)
  endif()
  add_test(NAME nxp_${name} COMMAND test_nxp_${name})
endforeach()
