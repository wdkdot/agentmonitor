cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Build only main and its transitive dependencies. This keeps the firmware free
# of unused Wi-Fi/Bluetooth stacks and also makes clean builds much faster.
idf_build_set_property(MINIMAL_BUILD ON)

project(ai_usage_monitor)
