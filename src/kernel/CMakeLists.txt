# silex_kernel — geometric core (OpenCascade). Its only public header is KernelEngine.h,
# which is OCCT-free: OCCT include directories are PRIVATE so that the UI physically
# cannot include TopoDS_Shape & co (architecture rule "no OCCT logic in the UI").
find_package(OpenCASCADE CONFIG REQUIRED)

add_library(silex_kernel STATIC
    KernelEngine.cpp
    NamedShape.cpp
    TopoNaming.cpp
    Feature.cpp
    Recompute.cpp
    Tessellator.cpp
    StepIO.cpp
    ops/OpSketch.cpp
    ops/OpExtrude.cpp
    ops/OpRevolve.cpp
    ops/OpBlend.cpp
    ops/OpImportStep.cpp
    ops/OpShell.cpp
    ops/OpCombine.cpp
    ops/OpMirror.cpp
    ops/OpPattern.cpp
    GeomRefs.cpp
)

target_include_directories(silex_kernel
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${OpenCASCADE_INCLUDE_DIR})

target_link_libraries(silex_kernel
    PUBLIC  silex_model
    PRIVATE ${OpenCASCADE_FoundationClasses_LIBRARIES}
            ${OpenCASCADE_ModelingData_LIBRARIES}
            ${OpenCASCADE_ModelingAlgorithms_LIBRARIES}
            ${OpenCASCADE_DataExchange_LIBRARIES})

# Internal headers are reachable by the regression tests through this interface target.
add_library(silex_kernel_internal INTERFACE)
target_include_directories(silex_kernel_internal INTERFACE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(silex_kernel_internal INTERFACE silex_kernel
    ${OpenCASCADE_FoundationClasses_LIBRARIES}
    ${OpenCASCADE_ModelingData_LIBRARIES}
    ${OpenCASCADE_ModelingAlgorithms_LIBRARIES}
    ${OpenCASCADE_DataExchange_LIBRARIES})
