@echo off
rem 编译并运行全部测试（Debug）：run_tests.bat
setlocal
call "%~dp0build.bat" debug || exit /b 1
ctest --preset debug || exit /b 1
python "%~dp0tools\extract_messages.py" --check || exit /b 1
python "%~dp0tools\make_icons.py" --check || exit /b 1
endlocal
