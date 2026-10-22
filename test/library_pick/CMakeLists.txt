add_executable(LibraryPickTest
  LibraryPickTest.cpp
)

target_include_directories(LibraryPickTest PRIVATE
  ${REPO_ROOT}/lib/LibraryIndex
)

target_link_libraries(LibraryPickTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LibraryPickTest)
