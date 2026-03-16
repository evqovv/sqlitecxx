cmake_minimum_required(VERSION 3.15.0)

project(sqlitecxx)

find_package(SQLite3 REQUIRED)

add_library(sqlitecxx INTERFACE)

target_include_directories(sqlitecxx INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(sqlitecxx INTERFACE cxx_std_20)

target_link_libraries(sqlitecxx INTERFACE SQLite::SQLite3)