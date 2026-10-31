cmake_minimum_required(VERSION 3.20)

# 版本号的唯一来源：发布时只改这里（MiniBIMVersion.h 由它生成，见 apps/Common：exe 版本信息、关于对话框、状态栏）
set(MINIBIM_VERSION 0.1.0)

project(MiniBIM VERSION ${MINIBIM_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 静态链接 MSVC 运行库（/MT、/MTd），拷到其他电脑上运行时不再依赖 MSVCP140(D).dll / VCRUNTIME140(D).dll
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Ninja + MSVC：头文件依赖靠解析 cl /showIncludes 的输出。中文版 cl 的前缀"注意: 包含文件:"按构建时控制台的代码页
# 输出（GBK / UTF-8），ninja 却只认配置时写进 rules.ninja 的那一种字节；对不上时 ninja 记下的依赖为空，改了头文件不重编，
# 新旧 .obj / .lib 混用，编译通过、运行时崩溃。VSLANG=1033 只在装了英文语言包时有效，所以这里经启动器 ClShowIncludes
# （cmake/ClShowIncludes.cpp，与 MiniCAD 相同）编译：它把各种编码的本地化前缀改写成 ASCII 的 "Note: including file:"。
if(CMAKE_GENERATOR MATCHES "Ninja" AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND CMAKE_CXX_CL_SHOWINCLUDES_PREFIX)
    string(REGEX REPLACE " +$" "" _clPrefix "${CMAKE_CXX_CL_SHOWINCLUDES_PREFIX}")
    if(NOT _clPrefix MATCHES "^[ -~]*$")   # 含非 ASCII 字符才需要
        set(_clLauncher "${CMAKE_BINARY_DIR}/ClShowIncludes.exe")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/cmake/ClShowIncludes.cpp")
        if(NOT EXISTS "${_clLauncher}" OR "${CMAKE_SOURCE_DIR}/cmake/ClShowIncludes.cpp" IS_NEWER_THAN "${_clLauncher}")
            try_compile(_clLauncherOk "${CMAKE_BINARY_DIR}/ClShowIncludes" "${CMAKE_SOURCE_DIR}/cmake/ClShowIncludes.cpp"
                        COMPILE_DEFINITIONS /utf-8 COPY_FILE "${_clLauncher}" OUTPUT_VARIABLE _clLauncherLog)
        endif()
        if(EXISTS "${_clLauncher}")
            string(HEX "${_clPrefix}" _clPrefixHex)
            set(CMAKE_CL_SHOWINCLUDES_PREFIX "Note: including file:")
            set(CMAKE_CXX_COMPILER_LAUNCHER "${_clLauncher}" "--prefix=${_clPrefixHex}" ${CMAKE_CXX_COMPILER_LAUNCHER})
        else()
            message(WARNING "ClShowIncludes 编译失败，头文件依赖可能因控制台代码页不同而失效：\n${_clLauncherLog}")
        endif()
    endif()
endif()

# 各库的头文件有自己的 include 根：MiniBIM → src，MiniGUI → src/UI，渲染后端 → apps/Render，共用应用 → apps/Common/src。
# 各根之下的 "目录/文件.h" 相对路径不能重复，否则会按搜索顺序互相遮挡（编译器不报错）。

# ── MiniGUI（保留模式界面库，复制自 MiniCAD）─────────────────────────
include(cmake/MiniGUI.cmake)

# ── SMLib（第三方实体建模内核，可选，MINIBIM_WITH_SOLID）────────────────
include(cmake/SMLib.cmake)

# ── MiniBIM（平台无关的核心库）───────────────────────────────────────
include(cmake/MiniBIM.cmake)

# ── Revit 文件读取器（Apache-2.0，可选，MINIBIM_WITH_RVT）────────────────
include(cmake/Rvt.cmake)

# ── 渲染后端（apps/Render：Windows 为 D3D12 / D3D11，Linux 为 Vulkan / GLES3，网页为 WebGPU / WebGL2）──
include(cmake/Render.cmake)

# ── 平台应用 ────────────────────────────────────────────────────────
add_subdirectory("apps/Common")     # 主窗口界面与逻辑（各平台宿主共用）

if(WIN32 AND NOT EMSCRIPTEN)
    add_subdirectory("apps/Win32")
endif()
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    add_subdirectory("apps/Linux")  # 缺 SDL3 或 OpenGL ES 3 时跳过（只编核心库与测试）
endif()
if(EMSCRIPTEN)
    add_subdirectory("apps/Web")    # build_wasm.bat
endif()

# ── 测试 ────────────────────────────────────────────────────────────
option(MINIBIM_BUILD_TESTS "Build MiniBIM tests" ON)
if(MINIBIM_BUILD_TESTS AND NOT EMSCRIPTEN)
    enable_testing()
    add_subdirectory(tests)
endif()
