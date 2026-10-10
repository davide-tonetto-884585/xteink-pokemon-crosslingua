add_executable(PokemonSleepRoomTest
  PokemonSleepRoomTest.cpp
  ${REPO_ROOT}/lib/Pokemon/PokemonSleepRoom.cpp
)

target_link_libraries(PokemonSleepRoomTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

add_test(NAME PokemonSleepRoomTest COMMAND PokemonSleepRoomTest)
