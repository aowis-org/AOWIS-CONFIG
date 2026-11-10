cmake_minimum_required(VERSION 3.20)
project(AOWISConfigInstalledConsumer LANGUAGES CXX)

find_package(AOWISConfig 0.1 CONFIG REQUIRED)
add_executable(aowis-config-installed-consumer ../consumer/main.cpp)
target_link_libraries(aowis-config-installed-consumer PRIVATE aowis::config)
