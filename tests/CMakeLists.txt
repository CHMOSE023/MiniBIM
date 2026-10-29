# ── 测试 ────────────────────────────────────────────────────────────
# I18n/   多语言：PO 解析、复数公式、语言代码、回退、Format，以及 assets/i18n 下的词典都能无警告读出 → I18nTests
# View/   三维视图的相机 → ViewTests
# Solid/  SMLib 冒烟测试（复制自 MiniCAD）→ SMLibSmokeTests（MINIBIM_WITH_SOLID 时）
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

if(MINIBIM_WITH_SOLID)
    add_executable(SMLibSmokeTests "Solid/SMLibSmokeTests.cpp")
    target_link_libraries(SMLibSmokeTests PRIVATE SM_API)
    if(MSVC)
        target_compile_options(SMLibSmokeTests PRIVATE /utf-8)
    endif()
    add_test(NAME SMLibSmokeTests COMMAND SMLibSmokeTests)
endif()

set_target_properties(I18nTests ViewTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(MINIBIM_WITH_SOLID)
    set_target_properties(SMLibSmokeTests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
