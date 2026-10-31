find_package(Qt6 REQUIRED COMPONENTS Test)

add_library(orbit_test_support STATIC TestHttpServer.cpp TestHttpServer.hpp)
target_link_libraries(orbit_test_support PUBLIC Qt6::Core Qt6::Network)

function(orbit_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE ${ARGN} orbit_test_support Qt6::Test)
    # Results go to the console and to <name>.log, which CI prints when a test fails.
    add_test(NAME ${name} COMMAND ${name} -o "${CMAKE_CURRENT_BINARY_DIR}/${name}.log,txt" -o -,txt)
    # Each test gets a private HOME/XDG tree, so nothing touches the real profile.
    set_tests_properties(${name} PROPERTIES
        TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;HOME=${CMAKE_CURRENT_BINARY_DIR}/home-${name};XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/home-${name}/.config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/home-${name}/.local/share;XDG_DOWNLOAD_DIR=${CMAKE_CURRENT_BINARY_DIR}/home-${name}/Downloads"
    )
endfunction()

orbit_add_test(tst_utils orbit_core)
orbit_add_test(tst_httpdownloader orbit_core)
orbit_add_test(tst_downloadmanager orbit_core)
orbit_add_test(tst_remoteserver orbit_services)
