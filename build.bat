@echo off
REM build.bat - rebuild pefiaOS.iso via the WSL i686-elf toolchain.
REM The cross-compiler lives at $HOME/opt/cross/bin inside WSL (see
REM toolchain/build-i686-elf.sh). This just runs `make` in that env.

wsl bash -c "cd \"$(wslpath '%CD%')\" && export PATH=\"$HOME/opt/cross/bin:$PATH\" && make %*"
if %ERRORLEVEL% neq 0 (
    echo.
    echo BUILD FAILED - see output above.
    exit /b %ERRORLEVEL%
)
echo.
echo Built pefiaOS.iso
