add_executable(aowis-config-core-tests config_core_test.cpp)
target_link_libraries(aowis-config-core-tests PRIVATE aowis::config)
add_test(NAME aowis-config-core COMMAND aowis-config-core-tests)

# Compile a separate executable through exactly the public target and headers;
# do not rely on any source-private include paths.
add_executable(aowis-config-consumer-smoke consumer/main.cpp)
target_link_libraries(aowis-config-consumer-smoke PRIVATE aowis::config)
add_test(NAME aowis-config-consumer-smoke COMMAND aowis-config-consumer-smoke)

add_executable(aowis-config-object-store-tests object_store_test.cpp)
target_link_libraries(aowis-config-object-store-tests PRIVATE aowis::config)
add_test(NAME aowis-config-object-store COMMAND aowis-config-object-store-tests)

add_executable(aowis-config-wireguard-profiles-tests wireguard_profiles_test.cpp)
target_link_libraries(aowis-config-wireguard-profiles-tests PRIVATE aowis::config)
add_test(NAME aowis-config-wireguard-profiles COMMAND aowis-config-wireguard-profiles-tests)

# Same behavioral contract for every backend available on this platform
# (always in-memory; native POSIX where implemented).
add_executable(aowis-config-backend-contract-tests backend_contract_test.cpp)
target_link_libraries(aowis-config-backend-contract-tests PRIVATE aowis::config)
# Internal headers: the key-value (browser) backend is tested natively on a
# fake store, since localStorage only exists in a browser.
target_include_directories(aowis-config-backend-contract-tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME aowis-config-backend-contract COMMAND aowis-config-backend-contract-tests)

# The browser backend's JavaScript (EM_JS bodies in src/browser_backend.cpp),
# executed under Node with fake localStorage and navigator.locks.
find_program(AOWIS_CONFIG_NODE NAMES node nodejs)
if(AOWIS_CONFIG_NODE)
    add_test(NAME aowis-config-browser-glue
             COMMAND "${AOWIS_CONFIG_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/browser_storage_glue_test.mjs"
                     "${PROJECT_SOURCE_DIR}/src/browser_backend.cpp")
endif()
