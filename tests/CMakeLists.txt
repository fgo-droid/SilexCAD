find_package(GTest CONFIG REQUIRED)

add_executable(silex_tests
    TestHelpers.h
    test_document.cpp
    test_sketch.cpp
    test_kernel.cpp
    test_tnp.cpp
    test_examples.cpp
    test_v02.cpp
    test_v03.cpp
    test_sketch_pro.cpp
    test_ergonomics.cpp
    test_v04.cpp
    test_v05.cpp
    TestPaths.h
    test_reliability.cpp
    test_vector_import.cpp
    test_text.cpp
    test_canvas.cpp
)
target_link_libraries(silex_tests PRIVATE silex_model silex_sketch silex_kernel_internal GTest::gtest_main)
# Source-tree fallbacks only: the tests first look next to their executable (TestPaths.h).
target_compile_definitions(silex_tests PRIVATE SILEX_SRC_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples"
                                              SILEX_SRC_DOCS_DIR="${PROJECT_SOURCE_DIR}/docs"
                                              SILEX_SRC_RESOURCES_DIR="${PROJECT_SOURCE_DIR}/resources"
                                              SILEX_SRC_TESTDATA_DIR="${PROJECT_SOURCE_DIR}/tests/data")
# Test data next to the executables (bin/<config>/testdata), so the folder is self-contained.
add_custom_command(TARGET silex_tests POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/tests/data" "$<TARGET_FILE_DIR:silex_tests>/testdata"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:silex_tests>/testdata/docs/manual"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/docs/manual/equerre.silex"
          "${PROJECT_SOURCE_DIR}/docs/manual/v02_mirror.silex" "$<TARGET_FILE_DIR:silex_tests>/testdata/docs/manual"
  VERBATIM)
if(TARGET silex)
  add_dependencies(silex_tests silex)  # silex deploys resources/ next to the executables
else()
  add_custom_command(TARGET silex_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:silex_tests>/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/examples" "$<TARGET_FILE_DIR:silex_tests>/resources/examples"
    VERBATIM)
endif()

include(GoogleTest)
gtest_discover_tests(silex_tests DISCOVERY_TIMEOUT 120)

# End-to-end IPC test: drives `silex --headless` through the local pipe.
if(SILEX_BUILD_APP)
  find_package(Qt6 REQUIRED COMPONENTS Core Network)
  add_executable(silex_ipc_tests test_ipc.cpp)
  target_link_libraries(silex_ipc_tests PRIVATE Qt6::Core Qt6::Network silex_model GTest::gtest_main)
  # fallback only: silex(.exe) is looked up first in $SILEX_EXE, then next to the test executable
  target_compile_definitions(silex_ipc_tests PRIVATE SILEX_BUILD_EXE="$<TARGET_FILE:silex>"
                                                     SILEX_SRC_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples")
  add_dependencies(silex_ipc_tests silex)
  gtest_discover_tests(silex_ipc_tests DISCOVERY_TIMEOUT 120 PROPERTIES TIMEOUT 300)

  # Sprint "Fiabilité" — RelocationTest: copies bin/<config> alone to an isolated folder, then runs
  # silex --doctor and the resource-dependent tests from there, without the source or build trees.
  add_test(NAME RelocationTest
           COMMAND powershell -NoProfile -ExecutionPolicy Bypass
                   -File "${PROJECT_SOURCE_DIR}/tools/relocation_test.ps1"
                   -BinDir "$<TARGET_FILE_DIR:silex>")
  set_tests_properties(RelocationTest PROPERTIES TIMEOUT 900 RUN_SERIAL TRUE)
endif()
