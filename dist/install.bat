@echo off
chcp 65001 >nul
cd /d "%~dp0"
if not exist "RaccoonCity.exe" (
    echo Put install.bat and the orcsteam folder into the game folder, next to RaccoonCity.exe.
    echo Положите install.bat и папку orcsteam в папку с игрой, где лежит RaccoonCity.exe.
    pause
    exit /b 1
)
rem Keep the original xlive.dll once; an empty backup means there was none.
if not exist "xlive.dll.orcsteam-backup" (
    if exist "xlive.dll" (
        copy /y "xlive.dll" "xlive.dll.orcsteam-backup" >nul
    ) else (
        type nul > "xlive.dll.orcsteam-backup"
    )
)
copy /y "orcsteam\xlive.dll" "xlive.dll" >nul
rem Leftovers of the test builds.
if exist "xlive_orig.dll" del "xlive_orig.dll"
if exist "orcsteam_xlive.log" del "orcsteam_xlive.log"
echo Done: orcsteam 1.1 is installed. The previous xlive.dll is saved as xlive.dll.orcsteam-backup
echo Готово: orcsteam 1.1 установлен. Прежний xlive.dll сохранён как xlive.dll.orcsteam-backup
pause
