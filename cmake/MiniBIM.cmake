# ── MiniBIM 核心库（平台无关，源码在 src，头文件以 src 为根："I18n/Translator.h"、"Core/Math/Vec3.hpp"）──
# 命名空间 MiniBIM。源文件逐个列出（不用通配），新增 .cpp 要加到这里。
# 移植 BIMCraft 的模型（bimcraft-core）时按 docs/PLAN.md 的分组往下加：Model/、Commands/、Ifc/ ……

add_library(MiniBIM STATIC
    # 多语言：PO 词典、当前语言（docs/i18n.md）
    src/I18n/Catalog.cpp
    src/I18n/Translator.cpp
    # 视图
    src/View/OrbitCamera.cpp
)

target_include_directories(MiniBIM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

if(MSVC)
    target_compile_options(MiniBIM PRIVATE /utf-8 /W4)
endif()

# ── 三维实体内核的适配层（唯一包含 SMLib 头文件的地方）─────────────────
# 接口 src/Solid/ISolidKernel.h 属于核心库（只用 MiniBIM 自己的类型）；SMLib 实现单独成库，私有链接 SM_API。
# 使用方链接 MiniBIMSolid（同时得到 MINIBIM_WITH_SOLID 宏）并调用 Solid::CreateSMLibKernel()；关掉 MINIBIM_WITH_SOLID 时不编译
if(MINIBIM_WITH_SOLID)
    add_library(MiniBIMSolid STATIC
        src/Solid/SMLib/SMLibKernel.cpp
    )
    target_link_libraries(MiniBIMSolid PUBLIC MiniBIM PRIVATE SM_API)
    target_compile_definitions(MiniBIMSolid PUBLIC MINIBIM_WITH_SOLID)
    if(MSVC)
        target_compile_options(MiniBIMSolid PRIVATE /utf-8 /W4)
    endif()
endif()
