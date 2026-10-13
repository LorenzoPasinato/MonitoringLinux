cmake_minimum_required(VERSION 3.16)

project(MonitoringLinux VERSION 1.0.0 LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite)
target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)

add_executable(monitoring-linux
    src/main.cpp
    src/Database.cpp
    src/HttpServer.cpp
)

target_include_directories(monitoring-linux PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(monitoring-linux PRIVATE sqlite3)

install(TARGETS monitoring-linux RUNTIME DESTINATION monitoring-linux)
install(FILES config/monitoring.conf DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/monitoring-linux)
install(DIRECTORY web/ DESTINATION monitoring-linux/web)

