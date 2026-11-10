add_executable(aowis-config-core-tests config_core_test.cpp)
target_link_libraries(aowis-config-core-tests PRIVATE aowis::config)
add_test(NAME aowis-config-core COMMAND aowis-config-core-tests)

# Compile a separate executable through exactly the public target and headers;
# do not rely on any source-private include paths.
add_executable(aowis-config-consumer-smoke consumer/main.cpp)
target_link_libraries(aowis-config-consumer-smoke PRIVATE aowis::config)
add_test(NAME aowis-config-consumer-smoke COMMAND aowis-config-consumer-smoke)
