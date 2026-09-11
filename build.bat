@echo off
setlocal

set "ROOT=%~dp0"
set "INCLUDE=%ROOT%include"
set "SRC=%ROOT%src"

if not exist "%ROOT%bin" mkdir "%ROOT%bin"

gcc -std=c11 -Wall -Wextra -I"%INCLUDE%" "%SRC%\sender.c" "%SRC%\capture.c" "%SRC%\input.c" "%SRC%\protocol.c" "%SRC%\transport_tcp.c" "%SRC%\transport_uart.c" "%SRC%\wic_jpeg.c" -o "%ROOT%bin\sender.exe" -lws2_32 -lgdi32 -luser32 -lole32 -loleaut32 -lwindowscodecs
if errorlevel 1 exit /b 1

gcc -std=c11 -Wall -Wextra -I"%INCLUDE%" "%SRC%\receiver.c" "%SRC%\protocol.c" "%SRC%\transport_tcp.c" "%SRC%\transport_uart.c" "%SRC%\wic_jpeg.c" -o "%ROOT%bin\receiver.exe" -lws2_32 -lgdi32 -luser32 -lole32 -loleaut32 -lwindowscodecs
if errorlevel 1 exit /b 1

endlocal