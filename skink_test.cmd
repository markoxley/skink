@echo off
REM Skink test-runner launcher for Windows.
REM The real harness is POSIX shell (skink_test.sh); this wrapper finds a
REM bash interpreter — Git for Windows' git-bash is the usual source —
REM and forwards all arguments.
setlocal
set "HERE=%~dp0"

for %%B in (bash.exe) do set "BASH=%%~$PATH:B"
if not defined BASH (
    for %%P in (
        "%ProgramFiles%\Git\bin\bash.exe"
        "%ProgramFiles%\Git\usr\bin\bash.exe"
        "%ProgramFiles(x86)%\Git\bin\bash.exe"
        "%LOCALAPPDATA%\Programs\Git\bin\bash.exe"
    ) do if not defined BASH if exist %%P set "BASH=%%~P"
)

if not defined BASH (
    echo skink_test: bash not found. Install Git for Windows ^(bundles bash.exe^) 1>&2
    echo   https://gitforwindows.org/ 1>&2
    exit /b 1
)

if not defined SKINK set "SKINK=%HERE%skink.exe"
"%BASH%" "%HERE%skink_test.sh" %*
exit /b %ERRORLEVEL%
