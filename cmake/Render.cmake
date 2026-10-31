# ── 渲染后端（源码在 apps/Render，头文件以它为根："D3D11/D3D11Backend.h"、"IGraphicsDevice.h"）──
# 复制自 MiniCAD 提交 0bd1687 的 apps/Render 与 cmake/Render.cmake。每个平台只编译自己的图形接口：
#   Windows → D3D12 + D3D11（WARP 软件设备作最后的回退）
#   Linux → Vulkan（默认）+ GLES3（EGL，回退；两者的窗口与上下文都由宿主经 SDL3 创建）
# 网页（WebGPU / WebGL2）的后端在 MiniCAD 的 apps/Render 里，做网页版时再复制过来（GraphicsDevice.cpp 已按 MINIBIM_RENDER_* 宏分派）。
# 界面后端（MiniGUI::IRenderBackend）各是一个小静态库；MiniBIMRender = 三维视口渲染器 + 宿主用的图形设备（IGraphicsDevice）。

set(MINIBIM_RENDER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/apps/Render")

# Linux：OpenGL ES 3 的头文件与库（Debian / Ubuntu 的 libgles-dev）；找不到时不编译 GLES3 后端与 Linux 宿主
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    find_path(MINIBIM_GLES3_INCLUDE_DIR GLES3/gl3.h)
    find_library(MINIBIM_GLESV2_LIBRARY GLESv2)
    if(MINIBIM_GLES3_INCLUDE_DIR AND MINIBIM_GLESV2_LIBRARY)
        set(MINIBIM_LINUX_GLES3 ON)
    else()
        message(STATUS "没有找到 OpenGL ES 3（GLES3/gl3.h、libGLESv2），不编译 GLES3 后端与 Linux 宿主")
    endif()

    # Vulkan：头文件与加载器（libvulkan-dev）+ 把 GLSL 编成 SPIR-V 的 glslangValidator（glslang-tools）；缺一个就只用 GLES3
    find_package(Vulkan QUIET)
    find_program(MINIBIM_GLSLANG NAMES glslangValidator glslang)
    if(Vulkan_FOUND AND MINIBIM_GLSLANG)
        set(MINIBIM_LINUX_VULKAN ON)
    else()
        message(STATUS "没有找到 Vulkan 开发文件或 glslangValidator，Linux 版只用 OpenGL ES 3")
    endif()
endif()

# Vulkan 的着色器：构建时把 apps/Render/Vulkan/Shaders 下的 GLSL 编成 SPIR-V 头文件（数组 kSpv_<文件名>，"." 换成 "_"），
# 放在构建目录的 generated/Spirv 下，代码里 #include "Spirv/line.vert.h"
if(MINIBIM_LINUX_VULKAN)
    set(MINIBIM_SPIRV_ROOT "${CMAKE_BINARY_DIR}/generated")
    set(MINIBIM_SPIRV_HEADERS)
    foreach(shader line.vert glyph.vert text.vert image.vert color.frag text.frag image.frag ui.vert ui.frag)
        string(REPLACE "." "_" var "${shader}")
        set(out "${MINIBIM_SPIRV_ROOT}/Spirv/${shader}.h")
        add_custom_command(
            OUTPUT  "${out}"
            COMMAND ${CMAKE_COMMAND} -E make_directory "${MINIBIM_SPIRV_ROOT}/Spirv"
            COMMAND ${MINIBIM_GLSLANG} -V --target-env vulkan1.3 --vn kSpv_${var} -o "${out}" "${MINIBIM_RENDER_SRC}/Vulkan/Shaders/${shader}"
            DEPENDS "${MINIBIM_RENDER_SRC}/Vulkan/Shaders/${shader}"
            COMMENT "SPIR-V：${shader}"
            VERBATIM
        )
        list(APPEND MINIBIM_SPIRV_HEADERS "${out}")
    endforeach()
    add_custom_target(MiniBIMSpirv DEPENDS ${MINIBIM_SPIRV_HEADERS})
endif()

# ── 界面后端 ─────────────────────────────────────────────────────────
if(WIN32 AND NOT EMSCRIPTEN)
    add_library(MiniGUI_D3D11 STATIC
        "${MINIBIM_RENDER_SRC}/D3D11/D3D11Backend.cpp"
    )
    target_include_directories(MiniGUI_D3D11 PUBLIC ${MINIBIM_RENDER_SRC})
    target_link_libraries(MiniGUI_D3D11
        PUBLIC  MiniGUI
        PRIVATE d3d11 D3DCompiler
    )
    if(MSVC)
        target_compile_options(MiniGUI_D3D11 PRIVATE /utf-8 /W4)
        target_compile_definitions(MiniGUI_D3D11 PRIVATE
            UNICODE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
    endif()

    # D3D12 界面后端；D3D12Context（设备、命令列表、上传缓冲、描述符）也在这里，视口渲染器共用它
    add_library(MiniGUI_D3D12 STATIC
        "${MINIBIM_RENDER_SRC}/D3D12/D3D12Backend.cpp"
        "${MINIBIM_RENDER_SRC}/D3D12/D3D12Context.cpp"
    )
    target_include_directories(MiniGUI_D3D12 PUBLIC ${MINIBIM_RENDER_SRC})
    target_link_libraries(MiniGUI_D3D12
        PUBLIC  MiniGUI d3d12 dxgi
        PRIVATE D3DCompiler
    )
    if(MSVC)
        target_compile_options(MiniGUI_D3D12 PRIVATE /utf-8 /W4)
        target_compile_definitions(MiniGUI_D3D12 PRIVATE
            UNICODE
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )
    endif()
endif()

if(MINIBIM_LINUX_GLES3)
    add_library(MiniGUI_GLES3 STATIC
        "${MINIBIM_RENDER_SRC}/GLES3/GLES3Backend.cpp"
    )
    target_include_directories(MiniGUI_GLES3 PUBLIC ${MINIBIM_RENDER_SRC})
    target_include_directories(MiniGUI_GLES3 PRIVATE ${MINIBIM_GLES3_INCLUDE_DIR})
    target_link_libraries(MiniGUI_GLES3 PUBLIC MiniGUI ${MINIBIM_GLESV2_LIBRARY})
endif()

# Vulkan 界面后端；VulkanContext（设备、命令缓冲、上传缓冲、描述符、动态渲染）也在这里，视口渲染器共用它
if(MINIBIM_LINUX_VULKAN)
    add_library(MiniGUI_Vulkan STATIC
        "${MINIBIM_RENDER_SRC}/Vulkan/VulkanBackend.cpp"
        "${MINIBIM_RENDER_SRC}/Vulkan/VulkanContext.cpp"
    )
    target_include_directories(MiniGUI_Vulkan
        PUBLIC  ${MINIBIM_RENDER_SRC}
        PRIVATE ${MINIBIM_SPIRV_ROOT}
    )
    target_link_libraries(MiniGUI_Vulkan PUBLIC MiniGUI Vulkan::Vulkan)
    add_dependencies(MiniGUI_Vulkan MiniBIMSpirv)
endif()

# 软件光栅后端（与平台无关，用于截图对比测试）
if(NOT EMSCRIPTEN)
    add_library(MiniGUI_Software STATIC
        "${MINIBIM_RENDER_SRC}/Software/SoftwareBackend.cpp"
    )
    target_include_directories(MiniGUI_Software PUBLIC ${MINIBIM_RENDER_SRC})
    target_link_libraries(MiniGUI_Software PUBLIC MiniGUI)
    if(MSVC)
        target_compile_options(MiniGUI_Software PRIVATE /utf-8 /W4)
    endif()
endif()

# ── 三维视口渲染器 + 图形设备 ─────────────────────────────────────────
add_library(MiniBIMRender STATIC
    "${MINIBIM_RENDER_SRC}/GraphicsDevice.cpp"
)
target_include_directories(MiniBIMRender PUBLIC ${MINIBIM_RENDER_SRC})
target_link_libraries(MiniBIMRender PUBLIC MiniBIM MiniGUI)

if(WIN32 AND NOT EMSCRIPTEN)
    target_sources(MiniBIMRender PRIVATE
        "${MINIBIM_RENDER_SRC}/D3D11/D3D11GraphicsDevice.cpp"
        "${MINIBIM_RENDER_SRC}/D3D11/D3D11Renderer.cpp"
        "${MINIBIM_RENDER_SRC}/D3D11/D3D11RenderTarget.cpp"
        "${MINIBIM_RENDER_SRC}/D3D11/Device.cpp"
        "${MINIBIM_RENDER_SRC}/D3D11/Shader.cpp"
        "${MINIBIM_RENDER_SRC}/D3D11/SwapChain.cpp"
    )
    target_compile_definitions(MiniBIMRender PRIVATE MINIBIM_RENDER_D3D11)
    target_link_libraries(MiniBIMRender PRIVATE MiniGUI_D3D11 d3d11 dxgi D3DCompiler)

    target_sources(MiniBIMRender PRIVATE
        "${MINIBIM_RENDER_SRC}/D3D12/D3D12GraphicsDevice.cpp"
        "${MINIBIM_RENDER_SRC}/D3D12/D3D12Renderer.cpp"
        "${MINIBIM_RENDER_SRC}/D3D12/D3D12RenderTarget.cpp"
    )
    target_compile_definitions(MiniBIMRender PRIVATE MINIBIM_RENDER_D3D12)
    target_link_libraries(MiniBIMRender PRIVATE MiniGUI_D3D12 d3d12)
endif()

if(MINIBIM_LINUX_GLES3)
    target_sources(MiniBIMRender PRIVATE
        "${MINIBIM_RENDER_SRC}/GLES3/GLES3GraphicsDevice.cpp"
        "${MINIBIM_RENDER_SRC}/GLES3/GLES3Renderer.cpp"
        "${MINIBIM_RENDER_SRC}/GLES3/GLRenderTarget.cpp"
    )
    target_compile_definitions(MiniBIMRender PRIVATE MINIBIM_RENDER_GLES3)
    target_include_directories(MiniBIMRender PRIVATE ${MINIBIM_GLES3_INCLUDE_DIR})
    target_link_libraries(MiniBIMRender PRIVATE MiniGUI_GLES3 ${MINIBIM_GLESV2_LIBRARY})
endif()

if(MINIBIM_LINUX_VULKAN)
    target_sources(MiniBIMRender PRIVATE
        "${MINIBIM_RENDER_SRC}/Vulkan/VulkanGraphicsDevice.cpp"
        "${MINIBIM_RENDER_SRC}/Vulkan/VulkanRenderer.cpp"
        "${MINIBIM_RENDER_SRC}/Vulkan/VulkanRenderTarget.cpp"
    )
    target_include_directories(MiniBIMRender PRIVATE ${MINIBIM_SPIRV_ROOT})
    target_compile_definitions(MiniBIMRender PRIVATE MINIBIM_RENDER_VULKAN)
    target_link_libraries(MiniBIMRender PRIVATE MiniGUI_Vulkan)
    add_dependencies(MiniBIMRender MiniBIMSpirv)
endif()

if(MSVC)
    target_compile_options(MiniBIMRender PRIVATE
        /utf-8
        /Z7                  # 调试信息嵌入 .obj，避免共享 PDB 锁定问题
    )
    target_compile_definitions(MiniBIMRender PRIVATE
        UNICODE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()
