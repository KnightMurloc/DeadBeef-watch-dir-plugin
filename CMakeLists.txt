cmake_minimum_required( VERSION 3.0 )
project(watch_dir C)

add_library(watch_dir SHARED lib.c utils.h utils.c)

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET glib-2.0)

target_link_libraries(watch_dir sqlite3 pthread rt PkgConfig::deps)