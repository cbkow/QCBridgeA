@echo off
rem build-setup.cmd — stage QCBridgeA.exe (self-contained: static runtime, the AJA SDK linked in), then Inno.
rem   Needs: a Release build in build-release (cmake -G Ninja), Inno Setup 6. Run from anywhere; paths are relative to this file.
setlocal
set "BUILD=%~dp0..\..\build-release"
set "STAGE=%~dp0..\..\build\stage"
if not exist "%BUILD%\QCBridgeA.exe" ( echo ERROR: build-release\QCBridgeA.exe missing & exit /b 1 )
if exist "%STAGE%" rmdir /s /q "%STAGE%"
mkdir "%STAGE%"
copy /y "%BUILD%\QCBridgeA.exe" "%STAGE%\" >nul
copy /y "%~dp0..\..\LICENSE" "%STAGE%\LICENSE.txt" >nul
rem The exe must not need the Visual C++ runtime DLLs: a user without the redistributable would get "VCRUNTIME140.dll was not found".
if not defined VSINSTALLDIR call "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
dumpbin /dependents "%STAGE%\QCBridgeA.exe" | findstr /i "VCRUNTIME MSVCP" >nul && ( echo ERROR: QCBridgeA.exe depends on the VC runtime DLLs - build with the static runtime & exit /b 1 )
"C:\Program Files (x86)\Inno Setup 6\ISCC.exe" /Q "%~dp0qcbridgea.iss" || ( echo ERROR: Inno failed & exit /b 1 )
echo SETUP OK
