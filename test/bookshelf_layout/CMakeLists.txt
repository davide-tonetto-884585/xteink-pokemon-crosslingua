add_executable(BookshelfLayoutTest
  BookshelfLayoutTest.cpp
  ${REPO_ROOT}/src/components/themes/bookshelf/BookshelfLayout.cpp
)

target_include_directories(BookshelfLayoutTest PRIVATE
  "${REPO_ROOT}/src"
)

target_link_libraries(BookshelfLayoutTest PRIVATE
  crosspoint_test_common
)

add_test(NAME BookshelfLayoutTest COMMAND BookshelfLayoutTest)
