# silex — Qt6 desktop application. Links the kernel only through KernelEngine.h
# (OCCT headers are not on this target's include path).
find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui GuiPrivate Widgets Network ShaderTools)
qt_standard_project_setup()

qt_add_executable(silex WIN32
    main.cpp
    Theme.h Theme.cpp
    Icons.h Icons.cpp
    Camera.h Camera.cpp
    Selection.h
    Picker.h Picker.cpp
    ModelController.h ModelController.cpp
    ViewportTool.h
    Viewport.h Viewport.cpp
    Workspace.h Workspace.cpp
    SketchEditor.h SketchEditor.cpp
    FeatureEditor.h FeatureEditor.cpp
    TransformEditor.h TransformEditor.cpp
    Timeline.h Timeline.cpp
    MarkingMenu.h MarkingMenu.cpp
    MainWindow.h MainWindow.cpp
    ../ipc/IpcServer.hpp ../ipc/IpcServer.cpp
    ../ipc/IpcCommandDispatcher.hpp ../ipc/IpcCommandDispatcher.cpp
)

qt_add_shaders(silex "silex_shaders"
    PREFIX "/"
    FILES
        shaders/mesh.vert
        shaders/mesh.frag
        shaders/oit.frag
        shaders/line.vert
        shaders/line.frag
        shaders/composite.vert
        shaders/composite.frag
)

target_link_libraries(silex PRIVATE Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Widgets Qt6::Network silex_model silex_sketch silex_kernel)
target_include_directories(silex PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Make the build directory runnable: Qt platform plugin next to the executable.
if(WIN32 AND DEFINED VCPKG_INSTALLED_DIR)
  add_custom_command(TARGET silex POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "$<IF:$<CONFIG:Debug>,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/Qt6/plugins/platforms,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins/platforms>"
            "$<TARGET_FILE_DIR:silex>/plugins/platforms"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "$<IF:$<CONFIG:Debug>,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/Qt6/plugins/styles,${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/Qt6/plugins/styles>"
            "$<TARGET_FILE_DIR:silex>/plugins/styles"
    COMMAND ${CMAKE_COMMAND} -E echo "[Paths]" > "$<TARGET_FILE_DIR:silex>/qt.conf"
    COMMAND ${CMAKE_COMMAND} -E echo "Plugins = plugins" >> "$<TARGET_FILE_DIR:silex>/qt.conf"
    VERBATIM)
endif()
