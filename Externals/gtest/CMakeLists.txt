include_directories(.)
add_definitions(-w)
add_library(gtest EXCLUDE_FROM_ALL src/gtest-all.cc src/gtest_main.cc)
