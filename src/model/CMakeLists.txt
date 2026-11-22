# silex_model — pure data layer (no OCCT, no Qt):
# parametric tree, immutable commands, undo/redo, JSON persistence, render meshes.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(silex_model STATIC
    Uuid.cpp
    Sketch.cpp
    Features.cpp
    Document.cpp
    Expression.cpp
    Material.cpp
    Bindings.cpp
    Printability.cpp
    Pedagogy.cpp
    Recovery.cpp
)
target_include_directories(silex_model PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(silex_model PUBLIC nlohmann_json::nlohmann_json)
