@echo off

rem === Deploy built programs. ZIP, check with Windows Defender and copy them to network shares =============================

for /f "delims=" %%# in ('powershell get-date -format "{yyyyMMdd-HHmm}"') do @set FILEDATE=%%#

echo Filedate %FILEDATE%

pushd "%APROJECTS%\deploy"

rem ===========================================================================
rem ==== Pack Little Navconnect ===================================================================
del LittleNavconnect.zip

"C:\Program Files\7-Zip\7z.exe" a LittleNavconnect.zip "Little Navconnect"
IF ERRORLEVEL 1 goto :err

"C:\Program Files\Windows Defender\MpCmdRun.exe" -Scan -ScanType 3 -DisableRemediation -File "%APROJECTS%\deploy\LittleNavconnect.zip"
IF ERRORLEVEL 1 goto :err

del \\sol\public\Releases\LittleNavconnect-%FILEDATE%.zip
copy /Y /Z /B LittleNavconnect.zip \\sol\public\Releases\LittleNavconnect-win-%FILEDATE%.zip
IF ERRORLEVEL 1 goto :err

rem ===========================================================================
rem ==== Pack Little Xpconnect ===================================================================
del LittleXpconnect.zip

"C:\Program Files\7-Zip\7z.exe" a LittleXpconnect.zip "Little Xpconnect"
IF ERRORLEVEL 1 goto :err

"C:\Program Files\Windows Defender\MpCmdRun.exe" -Scan -ScanType 3 -DisableRemediation -File "%APROJECTS%\deploy\LittleXpconnect.zip"
IF ERRORLEVEL 1 goto :err

del \\sol\public\Releases\LittleXpconnect-%FILEDATE%.zip
copy /Y /Z /B LittleXpconnect.zip \\sol\public\Releases\LittleXpconnect-win-%FILEDATE%.zip
IF ERRORLEVEL 1 goto :err

rem ===========================================================================
rem ==== Pack Little Navmap ===================================================================
del LittleNavmap.zip

"C:\Program Files\7-Zip\7z.exe" a LittleNavmap.zip "Little Navmap"
IF ERRORLEVEL 1 goto :err

"C:\Program Files\Windows Defender\MpCmdRun.exe" -Scan -ScanType 3 -DisableRemediation -File "%APROJECTS%\deploy\LittleNavmap.zip"
IF ERRORLEVEL 1 goto :err

del \\sol\public\Releases\LittleNavmap-%FILEDATE%.zip
copy /Y /Z /B LittleNavmap.zip \\sol\public\Releases\LittleNavmap-win-%FILEDATE%.zip
IF ERRORLEVEL 1 goto :err

popd

echo ---- Success ----

if not "%1" == "nopause" pause

exit /b 0

:err

echo **** ERROR ****

popd

pause

exit /b 1
