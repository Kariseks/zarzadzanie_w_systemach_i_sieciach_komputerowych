cmake_minimum_required(VERSION 3.16)
project(EdgeDetection LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Do pomiarow czasu zawsze kompiluj w Release (Debug jest kilka razy wolniejszy)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

add_executable(edge_detector
    main.cpp
    edge_detector.cpp
    edge_detector.h
    row_scheduler.h
    thread_pool.h
    common.h
    stb_impl.cpp
)

target_link_libraries(edge_detector PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(edge_detector PRIVATE psapi)
endif()

# Skopiuj input.jpg do katalogu budowania (tam Qt Creator uruchamia program)
if(EXISTS "${CMAKE_SOURCE_DIR}/input.jpg")
    configure_file("${CMAKE_SOURCE_DIR}/input.jpg"
                   "${CMAKE_BINARY_DIR}/input.jpg" COPYONLY)
endif()
