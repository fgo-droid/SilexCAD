find_package(GTest CONFIG REQUIRED)

add_executable(silex_tests
    TestHelpers.h
    test_document.cpp
    test_sketch.cpp
    test_kernel.cpp
    test_tnp.cpp
    test_examples.cpp
    test_v02.cpp
)
target_link_libraries(silex_tests PRIVATE silex_model silex_sketch silex_kernel_internal GTest::gtest_main)
target_compile_definitions(silex_tests PRIVATE SILEX_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples")

include(GoogleTest)
gtest_discover_tests(silex_tests DISCOVERY_TIMEOUT 120)

# End-to-end IPC test: drives `silex --headless` through the local pipe.
if(SILEX_BUILD_APP)
  find_package(Qt6 REQUIRED COMPONENTS Core Network)
  add_executable(silex_ipc_tests test_ipc.cpp)
  target_link_libraries(silex_ipc_tests PRIVATE Qt6::Core Qt6::Network silex_model GTest::gtest_main)
  target_compile_definitions(silex_ipc_tests PRIVATE SILEX_EXE="$<TARGET_FILE:silex>")
  add_dependencies(silex_ipc_tests silex)
  gtest_discover_tests(silex_ipc_tests DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 300)
endif()
