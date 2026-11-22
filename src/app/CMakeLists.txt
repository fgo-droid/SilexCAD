# silex — Qt6 desktop application. Links the kernel only through KernelEngine.h
# (OCCT headers are not on this target's include path).
find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui GuiPrivate Widgets Network ShaderTools)
qt_standard_project_setup()

qt_add_executable(silex WIN32
    main.cpp
    BuildInfo.h
    ResourceLocator.h ResourceLocator.cpp
    Doctor.h Doctor.cpp
    AutosaveManager.h AutosaveManager.cpp
    AboutDialog.h AboutDialog.cpp
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
    ViewCube.h ViewCube.cpp
    DirectEditor.h DirectEditor.cpp
    HoleEditor.h HoleEditor.cpp
    TextEditor.h TextEditor.cpp
    ComponentLibrary.h ComponentLibrary.cpp
    EnclosureWizard.h EnclosureWizard.cpp
    RichTips.h RichTips.cpp
    LessonRecorder.h LessonRecorder.cpp
    PrintTools.h PrintTools.cpp
    LearnPanel.h LearnPanel.cpp
    FeatureEditor.h FeatureEditor.cpp
    TransformEditor.h TransformEditor.cpp
    Timeline.h Timeline.cpp
    MarkingMenu.h MarkingMenu.cpp
    MainWindow.h MainWindow.cpp
    ExprSpinBox.h ExprSpinBox.cpp
    ParametersDialog.h ParametersDialog.cpp
    AnalysisTools.h AnalysisTools.cpp
    SectionPanel.h SectionPanel.cpp
    SweepLoftEditor.h SweepLoftEditor.cpp
    DrawingSheet.h DrawingSheet.cpp
    DrawingWindow.h DrawingWindow.cpp
    ../ipc/IpcServer.hpp ../ipc/IpcServer.cpp
    ../ipc/IpcCommandDispatcher.hpp ../ipc/IpcCommandDispatcher.cpp
)

# Version and source revision shown by --version, --doctor and Help > About.
find_package(Git QUIET)
set(SILEX_GIT_COMMIT "unknown")
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
                  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE _commit
                  RESULT_VARIABLE _rc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(_rc EQUAL 0 AND _commit)
    set(SILEX_GIT_COMMIT "${_commit}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
                    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE _dirty ERROR_QUIET)
    if(_dirty)
      string(APPEND SILEX_GIT_COMMIT "-modified")
    endif()
  else()
    set(SILEX_GIT_COMMIT "uncommitted")
  endif()
endif()
target_compile_definitions(silex PRIVATE SILEX_VERSION="${PROJECT_VERSION}" SILEX_GIT_COMMIT="${SILEX_GIT_COMMIT}")

# Legal texts shown by Help > About Silex / Licenses
qt_add_resources(silex "silex_legal"
    PREFIX "/legal"
    BASE "${PROJECT_SOURCE_DIR}"
    FILES
        ${PROJECT_SOURCE_DIR}/LICENSE
        ${PROJECT_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt
)

# v0.4 lot 3: lessons of the guided mode
qt_add_resources(silex "silex_tutorials"
    PREFIX "/"
    BASE "${PROJECT_SOURCE_DIR}/resources"
    FILES
        ${PROJECT_SOURCE_DIR}/resources/tutorials/01_equerre.json
        ${PROJECT_SOURCE_DIR}/resources/tutorials/02_boitier.json
        ${PROJECT_SOURCE_DIR}/resources/tutorials/03_bride.json
        ${PROJECT_SOURCE_DIR}/resources/tutorials/04_de.json
        ${PROJECT_SOURCE_DIR}/resources/tutorials/05_pince.json
        ${PROJECT_SOURCE_DIR}/resources/tutorials/06_poulie.json
        ${PROJECT_SOURCE_DIR}/resources/tutorials/07_palier.json
        ${PROJECT_SOURCE_DIR}/resources/tutorials/08_raccord.json
        ${PROJECT_SOURCE_DIR}/resources/tutorials/09_coude.json
        # v0.5 lot 2: repair challenges (lesson + broken part)
        ${PROJECT_SOURCE_DIR}/resources/tutorials/repair_1_esquisse_elastique.json
        ${PROJECT_SOURCE_DIR}/resources/tutorials/repair_1_esquisse_elastique.silex
        ${PROJECT_SOURCE_DIR}/resources/tutorials/repair_2_couvercle_bloque.json
        ${PROJECT_SOURCE_DIR}/resources/tutorials/repair_2_couvercle_bloque.silex
        ${PROJECT_SOURCE_DIR}/resources/tutorials/repair_3_conge_impossible.json
        ${PROJECT_SOURCE_DIR}/resources/tutorials/repair_3_conge_impossible.silex
        ${PROJECT_SOURCE_DIR}/resources/tutorials/repair_4_contour_ouvert.json
        ${PROJECT_SOURCE_DIR}/resources/tutorials/repair_4_contour_ouvert.silex
        ${PROJECT_SOURCE_DIR}/resources/tutorials/repair_5_piece_step.json
        ${PROJECT_SOURCE_DIR}/resources/tutorials/repair_5_piece_step.silex
)

# v0.5 lot 1: electronic components library (catalog metadata + simplified STEP models)
qt_add_resources(silex "silex_components"
    PREFIX "/"
    BASE "${PROJECT_SOURCE_DIR}/resources"
    FILES
        ${PROJECT_SOURCE_DIR}/resources/components/rpi4b.json
        ${PROJECT_SOURCE_DIR}/resources/components/rpi4b.step
        ${PROJECT_SOURCE_DIR}/resources/components/rpi5.json
        ${PROJECT_SOURCE_DIR}/resources/components/rpi5.step
        ${PROJECT_SOURCE_DIR}/resources/components/pico.json
        ${PROJECT_SOURCE_DIR}/resources/components/pico.step
        ${PROJECT_SOURCE_DIR}/resources/components/esp32_devkit.json
        ${PROJECT_SOURCE_DIR}/resources/components/esp32_devkit.step
        ${PROJECT_SOURCE_DIR}/resources/components/arduino_uno.json
        ${PROJECT_SOURCE_DIR}/resources/components/arduino_uno.step
        ${PROJECT_SOURCE_DIR}/resources/components/oled_096.json
        ${PROJECT_SOURCE_DIR}/resources/components/oled_096.step
        ${PROJECT_SOURCE_DIR}/resources/components/button_6x6.json
        ${PROJECT_SOURCE_DIR}/resources/components/button_6x6.step
        ${PROJECT_SOURCE_DIR}/resources/components/usb_c_receptacle.json
        ${PROJECT_SOURCE_DIR}/resources/components/usb_c_receptacle.step
        ${PROJECT_SOURCE_DIR}/resources/components/fan_40.json
        ${PROJECT_SOURCE_DIR}/resources/components/fan_40.step
)

# v0.7 lot A: bundled font of the parametric text (SIL Open Font License)
qt_add_resources(silex "silex_fonts"
    PREFIX "/"
    BASE "${PROJECT_SOURCE_DIR}/resources"
    FILES
        ${PROJECT_SOURCE_DIR}/resources/fonts/LiberationSans-Regular.ttf
        ${PROJECT_SOURCE_DIR}/resources/fonts/LiberationSans-OFL.txt
)

# annotated "cas d'école" opened from the Learn panel (guided tour)
qt_add_resources(silex "silex_examples"
    PREFIX "/"
    BASE "${PROJECT_SOURCE_DIR}"
    FILES
        ${PROJECT_SOURCE_DIR}/examples/equerre_annotee.silex
        ${PROJECT_SOURCE_DIR}/examples/de_parametrique.silex
        ${PROJECT_SOURCE_DIR}/examples/pince_serrage.silex
        ${PROJECT_SOURCE_DIR}/examples/poulie_gorge.silex
        ${PROJECT_SOURCE_DIR}/examples/palier_semelle.silex
        ${PROJECT_SOURCE_DIR}/examples/raccord_carre_rond.silex
        ${PROJECT_SOURCE_DIR}/examples/coude_brides.silex
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

# Self-contained bin/<config>/ folder (sprint "Fiabilité"): data files, MCP bridge and legal
# texts next to silex.exe, so that the folder can be copied anywhere (RelocationTest). The same
# files are embedded in the executable as a last resort (ResourceLocator).
add_custom_command(TARGET silex POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/resources/tutorials" "$<TARGET_FILE_DIR:silex>/resources/tutorials"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/resources/components" "$<TARGET_FILE_DIR:silex>/resources/components"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/resources/fonts" "$<TARGET_FILE_DIR:silex>/resources/fonts"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/examples" "$<TARGET_FILE_DIR:silex>/resources/examples"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:silex>/resources/legal"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt"
          "$<TARGET_FILE_DIR:silex>/resources/legal"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt"
          "$<TARGET_FILE_DIR:silex>"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${PROJECT_SOURCE_DIR}/tools/mcp_bridge/mcp_server.py" "${PROJECT_SOURCE_DIR}/tools/mcp_bridge/requirements.txt"
          "${PROJECT_SOURCE_DIR}/tools/mcp_bridge/mcp_config.example.json"
          "$<TARGET_FILE_DIR:silex>"
  VERBATIM)

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
