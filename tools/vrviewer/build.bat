@echo off
rem Builds MotorMashVR.exe (x64). Run from this folder.
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set VS=%%i
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /W3 /EHsc /MT /std:c++17 /I third_party\openxr\include vrviewer.cpp /Fe:MotorMashVR.exe /link /LIBPATH:third_party\openxr\native\x64\release\lib /SUBSYSTEM:WINDOWS || exit /b 1
copy /y third_party\openxr\native\x64\release\bin\openxr_loader.dll . >nul
