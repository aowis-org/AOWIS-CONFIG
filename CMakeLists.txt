cmake_minimum_required(VERSION 3.20)
project(AOWISConfig VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(aowis-config STATIC
    src/owner_id.cpp
    src/config_root.cpp
)
add_library(aowis::config ALIAS aowis-config)
set_target_properties(aowis-config PROPERTIES
    EXPORT_NAME config
    POSITION_INDEPENDENT_CODE ON
)
target_compile_features(aowis-config PUBLIC cxx_std_20)
target_include_directories(aowis-config PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(aowis-config PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS aowis-config EXPORT AOWISConfigTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

set(aowis_config_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/AOWISConfig")
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/AOWISConfigConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/AOWISConfigConfig.cmake"
    INSTALL_DESTINATION "${aowis_config_package_dir}"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/AOWISConfigConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)
install(EXPORT AOWISConfigTargets
    FILE AOWISConfigTargets.cmake
    NAMESPACE aowis::
    DESTINATION "${aowis_config_package_dir}"
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/AOWISConfigConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/AOWISConfigConfigVersion.cmake"
    DESTINATION "${aowis_config_package_dir}"
)

option(AOWIS_CONFIG_BUILD_TESTS "Build AOWIS-CONFIG tests" ${PROJECT_IS_TOP_LEVEL})
if(AOWIS_CONFIG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
