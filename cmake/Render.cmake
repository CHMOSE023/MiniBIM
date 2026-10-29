# ── 渲染后端（源码在 apps/Render，头文件以它为根："D3D11/D3D11Backend.h"、"IGraphicsDevice.h"）──
# 复制自 MiniCAD 提交 0bd1687 的 apps/Render。第一阶段只编 Windows：D3D12 + D3D11（WARP 软件设备作最后的回退）。
# 网页（WebGPU / WebGL2）与 Linux（Vulkan / GLES3）的后端在 MiniCAD 的 apps/Render 里，做到那两个平台时再复制过来，
# 并按 MiniCAD 的 cmake/Render.cmake 补上对应的段落（GraphicsDevice.cpp 已按 MINIBIM_RENDER_* 宏分派，不用改）。
# 界面后端（MiniGUI::IRenderBackend）各是一个小静态库；MiniBIMRender = 三维视口渲染器 + 宿主用的图形设备（IGraphicsDevice）。

set(MINIBIM_RENDER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/apps/Render")

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
