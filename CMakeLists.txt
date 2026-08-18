cmake_minimum_required(VERSION 3.15.0)

project(sqlitecxx LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SQLite3 3.37 REQUIRED)

if(NOT TARGET SQLite3::SQLite3)
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif()

enable_testing()

add_executable(test_sqlitecxx tests.cpp)

target_link_libraries(
    test_sqlitecxx
    PRIVATE SQLite3::SQLite3
)

add_test(
    NAME test_sqlitecxx
    COMMAND test_sqlitecxx
)