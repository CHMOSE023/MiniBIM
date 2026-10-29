# ── 测试 ────────────────────────────────────────────────────────────
# I18n/     多语言：PO 解析、复数公式、语言代码、回退、Format，以及 assets/i18n 下的词典都能无警告读出 → I18nTests
# View/     三维视图的相机 → ViewTests；模型经 SMLib 生成的三维场景 → ModelSceneTests（MINIBIM_WITH_SOLID 时）
# Solid/    SMLib 冒烟测试（复制自 MiniCAD）→ SMLibSmokeTests（MINIBIM_WITH_SOLID 时）
# Model/    模型核心（移植自 BIMCraft）：几何、单位、ID、元素与检查、墙、库 → ModelTests
# Commands/ 命令与会话（移植自 BIMCraft）：撤销、脚本、事务、建模、编辑、项目文件、示例房屋、恶意参数 → CommandTests
# Ifc/      IFC 交换（移植自 BIMCraft）：STEP 的读法、IFC4 导出、IFC2x3 / IFC4 导入与往返 → IfcTests
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
