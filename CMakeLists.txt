cmake_minimum_required(VERSION 3.16)

project(MonitoringLinux VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SQLite3 REQUIRED)

add_executable(monitoring-linux
    src/main.cpp
    src/Database.cpp
    src/HttpServer.cpp
)

target_include_directories(monitoring-linux PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(monitoring-linux PRIVATE SQLite::SQLite3)

install(TARGETS monitoring-linux RUNTIME DESTINATION sbin)
install(FILES config/monitoring.conf DESTINATION etc/monitoring-linux)
install(DIRECTORY web/ DESTINATION share/monitoring-linux)

