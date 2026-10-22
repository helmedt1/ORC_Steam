@echo off
chcp 65001 >nul
cd /d "%~dp0"
if not exist "xlive.dll.orcsteam-backup" (
    echo Backup xlive.dll.orcsteam-backup not found, nothing to remove.
    echo Резервная копия xlive.dll.orcsteam-backup не найдена, удалять нечего.
    pause
    exit /b 1
)
for %%A in ("xlive.dll.orcsteam-backup") do set "backupsize=%%~zA"
if "%backupsize%"=="0" (
    if exist "xlive.dll" del "xlive.dll"
) else (
    copy /y "xlive.dll.orcsteam-backup" "xlive.dll" >nul
)
del "xlive.dll.orcsteam-backup"
if exist "xlive_orig.dll" del "xlive_orig.dll"
echo Done: the game is back to its original state.
echo Готово: игра возвращена в исходное состояние.
pause
