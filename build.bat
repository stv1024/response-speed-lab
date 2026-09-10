@echo off
setlocal
cd /d "%~dp0"

set "VS=%ProgramFiles%\Microsoft Visual Studio\18\Community"
if not exist "%VS%\VC\Auxiliary\Build\vcvars64.bat" (
  echo [!] 找不到 Visual Studio 18 Community。请修改 build.bat 里的 VS 路径。
  exit /b 1
)

set "CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" set "CMAKE=cmake"

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

"%CMAKE%" -S . -B build -G "Visual Studio 18 2026" -A x64
if errorlevel 1 exit /b 1

"%CMAKE%" --build build --config Release --parallel
if errorlevel 1 exit /b 1

echo.
echo [ok] 已生成 bin\ResponseSpeedLab.exe
endlocal
