# ── MiniBIM 主窗口的界面与逻辑（各平台宿主共用）──────────────────────
# 平台相关的部分（窗口、图形设备、消息循环）由 AppPlatform 提供：Win32 版见 apps/Win32/src/Host
# 版本号由根 CMakeLists.txt 的 MINIBIM_VERSION 提供，生成 MiniBIMVersion.h：标题栏、关于对话框、状态栏、
# 桌面版 MiniBIM.rc 的 exe 版本信息共用。第四段构建号取 git 提交数，每次构建都重新生成（内容不变时不改写）
set(MINIBIM_VERSION_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated CACHE INTERNAL "MiniBIMVersion.h 所在目录")
set(MINIBIM_VERSION_HEADER ${MINIBIM_VERSION_DIR}/MiniBIMVersion.h)
add_custom_target(MiniBIMVersionHeader
    COMMAND ${CMAKE_COMMAND}
        -DVERSION=${MINIBIM_VERSION}
        -DSOURCE_DIR=${MiniBIM_SOURCE_DIR}
        -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/src/MiniBIMVersion.h.in
        -DOUTPUT=${MINIBIM_VERSION_HEADER}
        -P ${MiniBIM_SOURCE_DIR}/cmake/MiniBIMVersion.cmake
    BYPRODUCTS ${MINIBIM_VERSION_HEADER}
    COMMENT "生成 MiniBIMVersion.h"
    VERBATIM
)

add_library(MiniBIMApp STATIC
    src/GUI/MainFrame.cpp
    src/GUI/MainCommands.cpp
    src/GUI/PlanView.cpp
    src/GUI/Palettes.cpp
    src/GUI/Panels.cpp
    src/GUI/Tools.cpp
    src/GUI/Modify.cpp
    src/GUI/SketchMode.cpp
    src/GUI/Dialogs.cpp
    src/GUI/Parameters.cpp
    src/GUI/Recent.cpp
    src/GUI/Revit.cpp
    src/GUI/RevitReport.cpp
    src/GUI/StartPage.cpp
    src/GUI/Autosave.cpp
    src/GUI/Settings.cpp
    src/GUI/Shortcuts.cpp
    src/GUI/ShortcutsDialog.cpp
)

target_include_directories(MiniBIMApp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src ${MINIBIM_VERSION_DIR})
add_dependencies(MiniBIMApp MiniBIMVersionHeader)
target_link_libraries(MiniBIMApp PUBLIC MiniBIM MiniGUI)

if(MSVC)
    target_compile_options(MiniBIMApp PRIVATE /utf-8 /W4)
    target_compile_definitions(MiniBIMApp PRIVATE
        UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

if(NOT EMSCRIPTEN)
    # 开发时直接读源码目录里的界面描述文件：修改后立即重新加载，不必重新编译或复制
    target_compile_definitions(MiniBIMApp PRIVATE
        MINIBIM_UI_SOURCE="${CMAKE_SOURCE_DIR}/assets/ui/minibim_ui.json"
    )
endif()
