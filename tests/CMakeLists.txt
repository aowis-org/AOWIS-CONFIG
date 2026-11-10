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
