cmake_minimum_required(VERSION 3.5)
project(libwinhttppal VERSION 1.0.0 LANGUAGES CXX)

#Make sure that custom modules like FindRapidJSON are found
list(INSERT CMAKE_MODULE_PATH 0 ${CMAKE_SOURCE_DIR}/cmake)

##############################################
# Declare dependencies
find_package(CURL 7.57 REQUIRED MODULE)

##############################################
# Create target and set properties

add_library(winhttppal
    src/winhttppal.cpp
)

#Add an alias so that library can be used inside the build tree, e.g. when testing
add_library(winhttppal::winhttppal ALIAS winhttppal)

#Set target properties
target_include_directories(winhttppal
    PUBLIC
        $<INSTALL_INTERFACE:include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(winhttppal PRIVATE cxx_auto_type)
target_compile_options(winhttppal PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wall -fPIC>)

target_link_libraries(winhttppal
    PUBLIC
        ${CURL_LIBRARIES}
)

##############################################
# Installation instructions

include(GNUInstallDirs)
set(INSTALL_CONFIGDIR ${CMAKE_INSTALL_LIBDIR}/cmake/winhttppal)

install(TARGETS winhttppal
    EXPORT winhttppal-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

#This is required so that the exported target has the name winhttppal and not winhttppal
set_target_properties(winhttppal PROPERTIES EXPORT_NAME winhttppal)

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

#Export the targets to a script
install(EXPORT winhttppal-targets
  FILE
    winhttppalTargets.cmake
  NAMESPACE
    winhttppal::
  DESTINATION
    ${INSTALL_CONFIGDIR}
)

#Create a ConfigVersion.cmake file
include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/winhttppalConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

configure_package_config_file(${CMAKE_CURRENT_LIST_DIR}/cmake/winhttppalConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/winhttppalConfig.cmake
    INSTALL_DESTINATION ${INSTALL_CONFIGDIR}
)

#Install the config, configversion and custom find modules
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/winhttppalConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/winhttppalConfigVersion.cmake
    DESTINATION ${INSTALL_CONFIGDIR}
)

##############################################
## Exporting from the build tree
export(EXPORT winhttppal-targets FILE ${CMAKE_CURRENT_BINARY_DIR}/winhttppalTargets.cmake NAMESPACE winhttppal::)

#Register package in user's package registry
#export(PACKAGE winhttppal)

#add_subdirectory(test)