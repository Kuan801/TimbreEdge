@echo off
chcp 65001 >nul
cd /d "%~dp0"

where git >nul 2>nul
if errorlevel 1 (
  echo [X] git not found. Install Git for Windows first: https://git-scm.com/download/win
  pause
  exit /b 1
)

if exist ".git" (
  echo [!] .git already exists here. Nothing to do.
  pause
  exit /b 1
)

git init -b main
git config core.autocrlf false
git config i18n.commitEncoding utf-8
git config i18n.logOutputEncoding utf-8

if exist "提交訊息.txt" move /y "提交訊息.txt" ".git\INITIAL_MSG.txt" >nul

git add -A
git -c core.quotepath=false status --short

if exist ".git\INITIAL_MSG.txt" (
  git commit -F ".git\INITIAL_MSG.txt"
) else (
  git commit -m "initial commit: TimbreClone snapshot"
)

echo.
git -c core.quotepath=false log --stat --oneline -1
echo.
echo [OK] Done. This script can be deleted now.
pause
