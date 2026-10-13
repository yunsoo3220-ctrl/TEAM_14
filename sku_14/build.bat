@echo off
rem SKU Study Community - build with MSVC + MySQL C API + OpenCV (book photo barcode)
setlocal
set "MYSQL_DIR=C:\Program Files\MySQL\MySQL Server 8.0"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
rem OpenCV 4.8+ Windows release (https://opencv.org/releases) extracted to C:\opencv
if not defined OPENCV_DIR set "OPENCV_DIR=C:\opencv\build"
set "OPENCV_LIB=%OPENCV_DIR%\x64\vc16\lib"
set "OPENCV_BIN=%OPENCV_DIR%\x64\vc16\bin"

cd /d "%~dp0"
if not exist "%OPENCV_LIB%\opencv_world4*.lib" (echo [ERROR] OpenCV not found: %OPENCV_DIR% & exit /b 1)
for %%f in ("%OPENCV_LIB%\opencv_world4*.lib") do (
    echo %%~nf | findstr /e /c:"d " >nul || set "OPENCV_WORLD=%%~nf"
)
if not defined VCINSTALLDIR (
    call "%VCVARS%" >nul || (echo [ERROR] vcvars64.bat not found & exit /b 1)
)
if not exist build mkdir build

rem /MD: OpenCV DLL and our code share the same C runtime
cl /nologo /utf-8 /W3 /O2 /MD /EHsc /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
   /I"%MYSQL_DIR%\include" /I"%OPENCV_DIR%\include" src\*.c src\*.cpp /Fobuild\ /Fe:sku_server.exe ^
   /link /LIBPATH:"%MYSQL_DIR%\lib" /LIBPATH:"%OPENCV_LIB%" libmysql.lib %OPENCV_WORLD%.lib ^
   ws2_32.lib bcrypt.lib winhttp.lib
if errorlevel 1 (echo [ERROR] build failed & exit /b 1)

rem libmysql.dll and its OpenSSL dependencies, OpenCV DLL next to the exe
copy /y "%MYSQL_DIR%\lib\libmysql.dll" . >nul
copy /y "%MYSQL_DIR%\bin\libssl-3-x64.dll" . >nul
copy /y "%MYSQL_DIR%\bin\libcrypto-3-x64.dll" . >nul
copy /y "%OPENCV_BIN%\%OPENCV_WORLD%.dll" . >nul
echo [OK] sku_server.exe built. Run: sku_server.exe
