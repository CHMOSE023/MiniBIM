cmake_minimum_required(VERSION 3.20)
project(MiniBIMWin VERSION ${MINIBIM_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 渲染后端（MiniBIMRender、MiniGUI_D3D11、MiniGUI_D3D12）定义在 cmake/Render.cmake，源码在 apps/Render

# ── MiniBIM 桌面版：Win32 宿主（窗口 + 图形设备）+ 共用的主窗口界面（MiniBIMApp）──

# 版本号头文件 MiniBIMVersion.h 由 apps/Common 生成（目标 MiniBIMVersionHeader），MiniBIM.rc 用它填写 exe 版本信息

add_executable(MiniBIMWin
    src/Host/GuiMain.cpp
    src/Host/Win32Window.cpp
    src/MiniBIM.rc
)

# 资源编译器不报告 .ico 依赖：重新生成图标（tools/make_icons.py）后也要重编资源。
# /c65001：资源编译器默认按系统代码页（中文系统是 GBK）读 .rc 和它包含的头文件，UTF-8 的中文注释会吞掉换行（RC1004）
set_source_files_properties(src/MiniBIM.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/assets/icons/app.ico"
    COMPILE_OPTIONS "/c65001"
)

target_include_directories(MiniBIMWin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${MINIBIM_VERSION_DIR}
)

add_dependencies(MiniBIMWin MiniBIMVersionHeader)

target_link_libraries(MiniBIMWin PRIVATE
    MiniBIMApp
    MiniBIM
    MiniGUI_Win32
    MiniBIMRender
)

# 三维实体内核（SMLib）
if(MINIBIM_WITH_SOLID)
    target_link_libraries(MiniBIMWin PRIVATE MiniBIMSolid)
endif()

if(MSVC)
    target_compile_options(MiniBIMWin PRIVATE
        /utf-8
        /Z7                  # 调试信息嵌入 .obj，避免共享 PDB 锁定问题
        /FIpch.h             # 强制包含 Windows 头文件
    )
    target_compile_definitions(MiniBIMWin PRIVATE
        UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()

option(USE_WIN32 "Use Win GUI subsystem (no console)" OFF)
if(USE_WIN32)
    set_target_properties(MiniBIMWin PROPERTIES WIN32_EXECUTABLE TRUE)
    target_compile_definitions(MiniBIMWin PRIVATE USE_WIN32)
endif()

set_target_properties(MiniBIMWin PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_BINARY_DIR}/MiniBIMWin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_BINARY_DIR}/MiniBIMWin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/MiniBIMWin"
    VS_DEBUGGER_WORKING_DIRECTORY    "${CMAKE_BINARY_DIR}/MiniBIMWin"
)

# 复制共享资产（界面描述文件、词典）与许可文件到可执行文件旁
add_custom_command(TARGET MiniBIMWin POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:MiniBIMWin>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/LICENSE"
            "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
            "$<TARGET_FILE_DIR:MiniBIMWin>"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/3rd/SMLib/LICENSE"
            "$<TARGET_FILE_DIR:MiniBIMWin>/LICENSE-SMLib"
    COMMENT "Copy shared assets and licenses for MiniBIMWin"
)
