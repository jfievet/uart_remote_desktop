@echo off
setlocal

set "ROOT=%~dp0"
set "INCLUDE=%ROOT%include"
set "SRC=%ROOT%src"

if not exist "%ROOT%bin" mkdir "%ROOT%bin"

gcc -std=c11 -Wall -Wextra -D_WIN32_WINNT=0x0600 -DWINVER=0x0600 -I"%INCLUDE%" -I"%ROOT%third_party\stb" "%SRC%\sender.c" "%SRC%\capture.c" "%SRC%\capture_dxgi.c" "%SRC%\input.c" "%SRC%\protocol.c" "%SRC%\transport_tcp.c" "%SRC%\transport_uart.c" "%SRC%\wic_jpeg.c" -o "%ROOT%bin\sender.exe" -lws2_32 -lgdi32 -lgdiplus -luser32 -lole32 -loleaut32 -ld3d11 -ldxgi -ldxguid
if errorlevel 1 exit /b 1

gcc -std=c11 -Wall -Wextra -D_WIN32_WINNT=0x0600 -DWINVER=0x0600 -I"%INCLUDE%" -I"%ROOT%third_party\stb" "%SRC%\receiver.c" "%SRC%\protocol.c" "%SRC%\transport_tcp.c" "%SRC%\transport_uart.c" "%SRC%\wic_jpeg.c" -o "%ROOT%bin\receiver.exe" -lws2_32 -lgdi32 -lgdiplus -luser32 -lole32 -loleaut32
if errorlevel 1 exit /b 1

endlocal
