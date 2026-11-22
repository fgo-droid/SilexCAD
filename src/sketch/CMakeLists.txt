# silex_sketch — 2D constraint solving (PlaneGCS) and auto-constraint inference.
add_library(silex_sketch STATIC
    SketchSolver.cpp
    AutoConstraint.cpp
    Parametric.cpp
    SketchTools.cpp
    SketchCoach.cpp
    VectorImporter.cpp
)
target_include_directories(silex_sketch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(silex_sketch PUBLIC silex_model PRIVATE planegcs)
