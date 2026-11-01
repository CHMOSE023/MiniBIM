# ── MiniBIM 网页版（Emscripten）：界面由 MiniGUI 绘制，经 WebGPU 渲染，浏览器不支持时用 WebGL2（复制自 MiniCAD 的 apps/Web）──

# 渲染后端（MiniBIMRender、界面后端 MiniGUI_WebGPU / MiniGUI_GLES3）定义在 cmake/Render.cmake，源码在 apps/Render

# ── 网页宿主：画布、尺寸与按需刷新、WebGL2 上下文（用 WebGL2 时）──────────
add_library(MiniBIMWebHost STATIC
    src/Host/WebHost.cpp
)
target_include_directories(MiniBIMWebHost PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

set(MINIBIM_WEB_SHELL "${CMAKE_CURRENT_SOURCE_DIR}/shell/minigui_shell.html")

# ── MiniBIM 网页版：WebApp 宿主 + 共用的主窗口界面（MiniBIMApp）──────────
add_executable(MiniBIMWeb
    src/Host/WebMain.cpp
    src/Host/WebApp.cpp
)
target_include_directories(MiniBIMWeb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(MiniBIMWeb PRIVATE MiniBIMApp MiniBIM MiniGUI MiniGUI_Web MiniBIMRender MiniBIMWebHost)
# 三维实体内核（SMLib）：默认编入网页版，配置 -DMINIBIM_WITH_SOLID=OFF 可去掉（.wasm 小约 6 MB，但就没有三维模型了）
if(MINIBIM_WITH_SOLID)
    target_link_libraries(MiniBIMWeb PRIVATE MiniBIMSolid)
    target_compile_definitions(MiniBIMWeb PRIVATE MINIBIM_WITH_SOLID)
    # SMLib 的数值算法递归较深，emcc 默认 64 KB 的栈在建模时会溢出（表现为 call_indirect 报错），给 8 MB
    target_link_options(MiniBIMWeb PRIVATE -sSTACK_SIZE=8388608)
endif()
# Revit 文件读取器（Apache-2.0，cmake/Rvt.cmake）：得到宏 MINIBIM_WITH_RVT，WebMain.cpp 接上它
if(TARGET MiniBIMRvt)
    target_link_libraries(MiniBIMWeb PRIVATE MiniBIMRvt)
endif()
target_link_options(MiniBIMWeb PRIVATE
    -sMIN_WEBGL_VERSION=2
    -sMAX_WEBGL_VERSION=2
    -sALLOW_MEMORY_GROWTH=1
    -sEXPORTED_FUNCTIONS=_main,_malloc,_free
    -sENVIRONMENT=web
    "SHELL:--shell-file ${MINIBIM_WEB_SHELL}"
    # 资源打包到虚拟文件系统根目录：/ui（界面描述文件）、/i18n（词典）、/icons、/fonts（界面字体）
    # 同一个选项出现多次时 CMake 会去重，用 SHELL: 保持成对
    "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/assets/ui@/ui"
    "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/assets/i18n@/i18n"
    "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/assets/icons@/icons"
    "SHELL:--preload-file ${CMAKE_SOURCE_DIR}/assets/fonts@/fonts"
)
set_target_properties(MiniBIMWeb PROPERTIES
    OUTPUT_NAME "index"
    SUFFIX ".html"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/MiniBIMWeb"
    LINK_DEPENDS "${MINIBIM_WEB_SHELL}"
)
# 给 .js / .wasm / .data 的地址加内容哈希参数，发版后浏览器不会混用新旧文件
add_custom_command(TARGET MiniBIMWeb POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DHTML=$<TARGET_FILE:MiniBIMWeb> -P "${CMAKE_SOURCE_DIR}/cmake/WebCacheBust.cmake"
    VERBATIM
)
