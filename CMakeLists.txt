cmake_minimum_required(VERSION 3.20)
project(AOWISConfig VERSION 0.7.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(aowis_config_sources
    src/owner_id.cpp
    src/config_root.cpp
    src/object_id.cpp
    src/json.cpp
    src/json_string.cpp
    src/wireguard_profiles.cpp
    src/object_store.cpp
    src/memory_backend.cpp
    src/key_value_backend.cpp
)
# Exactly one persistent backend per platform (contract: src/storage_backend.h).
# The Windows backend replaces unsupported_backend.cpp when implemented.
if(EMSCRIPTEN)
    list(APPEND aowis_config_sources src/browser_backend.cpp)
elseif(WIN32)
    list(APPEND aowis_config_sources src/unsupported_backend.cpp)
else()
    list(APPEND aowis_config_sources src/posix_backend.cpp)
endif()
add_library(aowis-config STATIC ${aowis_config_sources})
add_library(aowis::config ALIAS aowis-config)
set_target_properties(aowis-config PROPERTIES
    EXPORT_NAME config
    POSITION_INDEPENDENT_CODE ON
)
target_compile_features(aowis-config PUBLIC cxx_std_20)
# MemoryStorage uses std::timed_mutex. Emscripten is left untouched so a
# single-threaded browser build is never switched to -pthread.
if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    target_link_libraries(aowis-config PUBLIC Threads::Threads)
endif()
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
    if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR AND UNIX AND NOT EMSCRIPTEN)
        # Never present in installed/release builds: deterministic SIGKILL
        # points exercise interrupted writes in a disposable test checkout.
        target_compile_definitions(aowis-config PRIVATE AOWIS_CONFIG_TEST_FAULT_INJECTION)
    endif()
    enable_testing()
    add_subdirectory(tests)
endif()
