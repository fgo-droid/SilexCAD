# PlaneGCS — geometric constraint solver extracted from FreeCAD 1.0.0
# (src/Mod/Sketcher/App/planegcs), LGPL-2.1-or-later. Only includes were patched.
find_package(Eigen3 CONFIG REQUIRED)
find_package(Boost REQUIRED COMPONENTS graph)
add_library(planegcs STATIC
    Constraints.cpp GCS.cpp Geo.cpp SubSystem.cpp qp_eq.cpp)
target_include_directories(planegcs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
                                    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat)
target_compile_definitions(planegcs PUBLIC EIGEN_NO_DEBUG _USE_MATH_DEFINES NOMINMAX)
target_link_libraries(planegcs PUBLIC Eigen3::Eigen Boost::graph)
if(MSVC)
  target_compile_options(planegcs PRIVATE /bigobj /W0)
else()
  target_compile_options(planegcs PRIVATE -w)
endif()
