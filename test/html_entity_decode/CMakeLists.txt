# Same harness shape as text_normalize: own main(), pass/fail via exit code.
add_executable(HtmlEntityDecodeTest
  HtmlEntityDecodeTest.cpp
  ${REPO_ROOT}/src/modules/lingua/utils/HtmlEntityDecode.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
)

# Sources include "modules/lingua/utils/HtmlEntityDecode.h" (root src/) and <Epub/htmlEntities.h> (root lib/Epub).
target_include_directories(HtmlEntityDecodeTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(HtmlEntityDecodeTest PRIVATE
  crosspoint_test_common
)

add_test(NAME HtmlEntityDecodeTest COMMAND HtmlEntityDecodeTest)
