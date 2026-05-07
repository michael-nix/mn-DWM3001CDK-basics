cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(mn-DWM3001CDK-basics)

add_subdirectory(lib/dwt_uwb_driver)

target_sources(app PRIVATE src/main.c)

target_link_libraries(app PRIVATE uwb_driver)
