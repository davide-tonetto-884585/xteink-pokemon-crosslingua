# TextNormalize has its own main() and reports pass/fail via exit code, so it is
# built as a plain executable and registered with CTest directly (no GTest).
add_executable(TextNormalizeTest
  TextNormalizeTest.cpp
  ${REPO_ROOT}/src/modules/lingua/utils/TextNormalize.cpp
)

target_include_directories(TextNormalizeTest PRIVATE
  ${REPO_ROOT}/src/modules/lingua/utils
)

target_link_libraries(TextNormalizeTest PRIVATE
  crosspoint_test_common
)

add_test(NAME TextNormalizeTest COMMAND TextNormalizeTest)
