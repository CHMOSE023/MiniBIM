# ── 测试 ────────────────────────────────────────────────────────────
# I18n/     多语言：PO 解析、复数公式、语言代码、回退、Format，以及 assets/i18n 下的词典都能无警告读出 → I18nTests
# View/     三维视图的相机 → ViewTests；模型经 SMLib 生成的三维场景 → ModelSceneTests（MINIBIM_WITH_SOLID 时）
# Solid/    SMLib 冒烟测试（复制自 MiniCAD）→ SMLibSmokeTests（MINIBIM_WITH_SOLID 时）
# Model/    模型核心（移植自 BIMCraft）：几何、单位、ID、元素与检查、墙、库 → ModelTests
# Commands/ 命令与会话（移植自 BIMCraft）：撤销、脚本、事务、建模、编辑、项目文件、示例房屋、恶意参数 → CommandTests
# Ifc/      IFC 交换（移植自 BIMCraft）：STEP 的读法、IFC4 导出、IFC2x3 / IFC4 导入与往返 → IfcTests
# App/      主窗口里不碰界面控件的部分（apps/Common/src/GUI/Parameters.cpp、Recent.cpp、Shortcuts.cpp，直接编进来，只依赖核心库）：特性面板的参数、项目浏览器的搜索、最近打开、
#           两个字母的快捷键 → AppTests
# 测试程序都是普通的可执行文件：打印 [PASS] / [FAIL]，有失败时返回非 0；输出到构建目录根（out/<preset>/），与 ctest 一起使用

add_executable(I18nTests "I18n/I18nTests.cpp")
target_link_libraries(I18nTests PRIVATE MiniBIM)
target_compile_definitions(I18nTests PRIVATE MINIBIM_I18N_DIR="${CMAKE_SOURCE_DIR}/assets/i18n")
if(MSVC)
    target_compile_options(I18nTests PRIVATE /utf-8)
endif()
add_test(NAME I18nTests COMMAND I18nTests)

add_executable(ViewTests "View/OrbitCameraTests.cpp")
target_link_libraries(ViewTests PRIVATE MiniBIM)
if(MSVC)
    target_compile_options(ViewTests PRIVATE /utf-8)
endif()
add_test(NAME ViewTests COMMAND ViewTests)

add_executable(ModelTests "Model/ModelTests.cpp")
target_link_libraries(ModelTests PRIVATE MiniBIM)
if(MSVC)
    target_compile_options(ModelTests PRIVATE /utf-8)
endif()
add_test(NAME ModelTests COMMAND ModelTests)

add_executable(CommandTests "Commands/CommandTests.cpp")
target_link_libraries(CommandTests PRIVATE MiniBIM)
if(MSVC)
    target_compile_options(CommandTests PRIVATE /utf-8)
endif()
add_test(NAME CommandTests COMMAND CommandTests)

add_executable(IfcTests "Ifc/IfcTests.cpp")
target_link_libraries(IfcTests PRIVATE MiniBIM)
if(MSVC)
    target_compile_options(IfcTests PRIVATE /utf-8)
endif()
add_test(NAME IfcTests COMMAND IfcTests)

add_executable(AppTests "App/AppTests.cpp" "${CMAKE_SOURCE_DIR}/apps/Common/src/GUI/Parameters.cpp"
    "${CMAKE_SOURCE_DIR}/apps/Common/src/GUI/Recent.cpp" "${CMAKE_SOURCE_DIR}/apps/Common/src/GUI/Shortcuts.cpp"
    "${CMAKE_SOURCE_DIR}/apps/Common/src/GUI/RevitReport.cpp")
target_include_directories(AppTests PRIVATE "${CMAKE_SOURCE_DIR}/apps/Common/src")
target_link_libraries(AppTests PRIVATE MiniBIM)
if(MSVC)
    target_compile_options(AppTests PRIVATE /utf-8)
endif()
add_test(NAME AppTests COMMAND AppTests)

if(MINIBIM_WITH_SOLID)
    add_executable(SMLibSmokeTests "Solid/SMLibSmokeTests.cpp")
    target_link_libraries(SMLibSmokeTests PRIVATE SM_API)
    if(MSVC)
        target_compile_options(SMLibSmokeTests PRIVATE /utf-8)
    endif()
    add_test(NAME SMLibSmokeTests COMMAND SMLibSmokeTests)

    add_executable(ModelSceneTests "View/ModelSceneTests.cpp")
    target_link_libraries(ModelSceneTests PRIVATE MiniBIMSolid)
    if(MSVC)
        target_compile_options(ModelSceneTests PRIVATE /utf-8)
    endif()
    add_test(NAME ModelSceneTests COMMAND ModelSceneTests)
endif()

set_target_properties(I18nTests ViewTests ModelTests CommandTests IfcTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(MINIBIM_WITH_SOLID)
    set_target_properties(SMLibSmokeTests ModelSceneTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()

# Rvt/      Revit 文件读取器（移植自 BIMCraft crates/rvt，Apache-2.0）：复合文档、gzip 与页、BasicFileInfo、Atom 条目、预览图、
#           类表、记录与清单（RecordTests.cpp）、元素数据的读法与草图（DecoderTests.cpp）、导入成模型（ImportTests.cpp 的标高与墙，
#           FloorTests.cpp 的楼板、天花板与屋顶，OpeningTests.cpp 的门窗、房间与柱）、许可的隔离 → RvtTests（MINIBIM_WITH_RVT 时）。
#           样例在测试里现做（Fixture.cpp 的容器，ProjectFixture.cpp 的合成项目，ImportFixture.cpp 的墙与草图），不用 Autodesk 的文件
if(MINIBIM_WITH_RVT)
    add_executable(RvtTests "Rvt/RvtTests.cpp" "Rvt/Fixture.cpp" "Rvt/ProjectFixture.cpp" "Rvt/RecordTests.cpp" "Rvt/DecoderTests.cpp"
                            "Rvt/ImportFixture.cpp" "Rvt/ImportTests.cpp" "Rvt/FloorTests.cpp" "Rvt/OpeningTests.cpp")
    # 主窗口里不碰界面控件的 RevitReport（数略去了几个）与它要的类别名称：读取器的说明和窗口的数法对得上
    target_sources(RvtTests PRIVATE "${CMAKE_SOURCE_DIR}/apps/Common/src/GUI/RevitReport.cpp" "${CMAKE_SOURCE_DIR}/apps/Common/src/GUI/Parameters.cpp")
    target_include_directories(RvtTests PRIVATE "${CMAKE_SOURCE_DIR}/apps/Common/src")
    target_link_libraries(RvtTests PRIVATE MiniBIMRvt)
    target_compile_definitions(RvtTests PRIVATE MINIBIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    if(MSVC)
        target_compile_options(RvtTests PRIVATE /utf-8)
    endif()
    add_test(NAME RvtTests COMMAND RvtTests)
    set_target_properties(RvtTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
