# Revit 文件读取器（src/Rvt，目标 MiniBIMRvt）：移植自 BIMCraft 的 crates/rvt，Apache-2.0（不是 MIT），见 src/Rvt/NOTICE。
# 核心库 MiniBIM 不依赖它；宿主链接 MiniBIMRvt 后得到宏 MINIBIM_WITH_RVT，打开对话框里才有 .rvt。
# 关掉开关（-DMINIBIM_WITH_RVT=OFF）时不编译，发行包里也就没有 Apache-2.0 的这一部分（SMLib 仍是 Apache-2.0）
option(MINIBIM_WITH_RVT "Build the Revit file reader (Apache-2.0, src/Rvt)" ON)

if(MINIBIM_WITH_RVT)
    add_library(MiniBIMRvt STATIC
        # 底层（R3）：字节、自己写的 inflate 与复合文档读取、gzip、容器，以及不用解析记录就能读出的元数据
        src/Rvt/Bytes.cpp
        src/Rvt/Inflate.cpp
        src/Rvt/Cfb.cpp
        src/Rvt/Gzip.cpp
        src/Rvt/Container.cpp
        src/Rvt/Info.cpp
        src/Rvt/Atom.cpp
        src/Rvt/Preview.cpp
    )
    target_include_directories(MiniBIMRvt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_definitions(MiniBIMRvt PUBLIC MINIBIM_WITH_RVT)
    if(MSVC)
        target_compile_options(MiniBIMRvt PRIVATE /utf-8 /W4)
    endif()
endif()
