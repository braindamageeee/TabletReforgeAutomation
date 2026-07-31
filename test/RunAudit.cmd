@echo off
REM RunAudit.cmd — 双击启动审计脚本（P2）
setlocal
cd /d "%~dp0"
PowerShell -NoProfile -ExecutionPolicy Bypass -File "%~dp0RunAudit.ps1"
echo.
echo [审计退出码: %ERRORLEVEL%]
pause
endlocal
