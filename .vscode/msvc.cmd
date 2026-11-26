@echo off
rem Configures and builds the Windows emulator with MSVC, for a developer
rem working in WSL: the emu: Build (Windows) task runs it through cmd.exe on
rem the \\wsl.localhost path of the repository.
rem
rem usage: msvc.cmd <preset>, a windows-* preset of
rem src/host/sokol/CMakePresets.json. The build goes where the preset puts
rem it, build/sokol/windows/<type>.
setlocal
rem pushd maps the UNC path to a drive letter, since cmd.exe cannot have a
rem UNC current directory.
pushd "%~dp0..\src\host\sokol" || exit /b 1
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo msvc.cmd: Visual Studio with C++ is not installed
    popd & exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo msvc.cmd: Visual Studio has no C++ build tools
    popd & exit /b 1
)
call "%VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 -no_logo || (popd & exit /b 1)
cmake --preset %1 || (popd & exit /b 1)
cmake --build --preset %1 || (popd & exit /b 1)
popd
