add_executable(TerminalTest
  TerminalTest.cpp
  ${REPO_ROOT}/src/terminal/TerminalScreen.cpp
  ${REPO_ROOT}/src/terminal/TerminalParser.cpp
)

target_include_directories(TerminalTest PRIVATE
  ${REPO_ROOT}/src/terminal
)

target_link_libraries(TerminalTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TerminalTest)
