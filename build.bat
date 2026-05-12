@echo off
mkdir build 2>nul
gcc -Wall -Wextra -O2 -mwindows -o build\codex-petdex-win.exe ^
    src\main.c src\pet.c src\sprite.c src\animation.c src\window.c ^
    -lole32 -lwindowscodecs -luuid
