# ArduinoJson comes from PlatformIO's libdeps; any environment's copy works. Skipped (not failed)
# when no environment has been built yet.
file(GLOB CHAPTER_RECAP_ARDUINOJSON_DIRS "${REPO_ROOT}/.pio/libdeps/*/ArduinoJson/src")
if(NOT CHAPTER_RECAP_ARDUINOJSON_DIRS)
  message(STATUS "ChapterRecapTest skipped: ArduinoJson not found under .pio/libdeps (run pio once)")
  return()
endif()
list(GET CHAPTER_RECAP_ARDUINOJSON_DIRS 0 CHAPTER_RECAP_ARDUINOJSON_DIR)

add_executable(ChapterRecapTest
  ChapterRecapTest.cpp
  ${REPO_ROOT}/src/modules/recap/RecapText.cpp
  ${REPO_ROOT}/src/modules/recap/GeminiRecap.cpp
)

target_include_directories(ChapterRecapTest PRIVATE
  ${REPO_ROOT}/src
  ${CHAPTER_RECAP_ARDUINOJSON_DIR}
)

target_link_libraries(ChapterRecapTest PRIVATE
  crosspoint_test_common
)

add_test(NAME ChapterRecapTest COMMAND ChapterRecapTest)
