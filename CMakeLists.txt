cmake_minimum_required(VERSION 2.8.9)
project(winhttppal DESCRIPTION "WinHttpPAL")

# dependencies
set(CURL_LIBRARY "-lcurl")
find_package(CURL REQUIRED)
include_directories(${CURL_INCLUDE_DIR})

# include path
include_directories(inc)

# definition
add_library(winhttppal SHARED src/winhttppal.cpp)
add_library(winhttppal-static src/winhttppal.cpp)

set_target_properties(winhttppal PROPERTIES
    SOVERSION 1
    PUBLIC_HEADER inc/winhttppal.h)

#compile flags
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O2" )

target_link_libraries(winhttppal pthread dl stdc++ ${CURL_LIBRARIES} crypto ssl)
target_link_libraries(winhttppal-static pthread dl stdc++ ${CURL_LIBRARIES} crypto ssl)
