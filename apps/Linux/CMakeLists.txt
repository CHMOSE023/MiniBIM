# ── MiniBIM Linux 版：SDL3 宿主（窗口、事件、Vulkan 表面或 EGL / GLES3 上下文）+ 共用的主窗口界面（MiniBIMApp）──
# 渲染走 VulkanGraphicsDevice，不行时 GLES3GraphicsDevice（cmake/Render.cmake 的 Linux 段）；平台层 MiniGUI_SDL 定义在 cmake/MiniGUI.cmake。
# 需要 SDL3 与 OpenGL ES 3 开发文件（Vulkan 可选），缺任何一个时只编译核心库与测试（见 README 的 Linux 构建说明）

if(NOT TARGET MiniGUI_SDL OR NOT MINIBIM_LINUX_GLES3)
    message(STATUS "MiniBIMLinux：缺少 SDL3 或 OpenGL ES 3，跳过")
    return()
endif()

add_executable(MiniBIMLinux
    src/Host/LinuxMain.cpp
    src/Host/LinuxApp.cpp
)

target_include_directories(MiniBIMLinux PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(MiniBIMLinux PRIVATE
    MiniBIMApp
    MiniBIM
    MiniGUI_SDL
    MiniBIMRender
    SDL3::SDL3
)

# 三维实体内核（SMLib）
if(MINIBIM_WITH_SOLID)
    target_link_libraries(MiniBIMLinux PRIVATE MiniBIMSolid)
    target_compile_definitions(MiniBIMLinux PRIVATE MINIBIM_WITH_SOLID)
endif()

set_target_properties(MiniBIMLinux PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/MiniBIMLinux"
)

# 复制共享资产（界面描述文件、词典、图标、字体）到可执行文件旁，与桌面版相同
add_custom_command(TARGET MiniBIMLinux POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:MiniBIMLinux>"
    COMMENT "Copy shared assets for MiniBIMLinux"
)
