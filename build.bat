@echo off
rem 配置并编译：build.bat [debug^|release]（默认 debug）。不在 VS 开发环境里时先用 vswhere 找到 VS 并调用 vcvars64.bat
rem 已经在"Developer Command Prompt for VS"里时也可以直接用 cmake --preset debug ^&^& cmake --build --preset debug
setlocal
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=debug"

where cl >nul 2>nul && goto build
rem 路径里有 "(x86)"，不能放在 if ( … ) 块里展开；for /f 会剥掉命令首尾的引号，所以命令前加 call
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto build
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if defined VSDIR call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul

:build
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
endlocal
