# codex-petdex-win

A lightweight Windows desktop virtual pet application built with pure Win32 API and WIC (Windows Imaging Component).

## Features

- Ultra-compact executable (< 200KB)
- WebP sprite animation with alpha transparency
- Transparent layered window (no title bar)
- Multiple pet states: Idle, Running, Jumping, Waving, Waiting, Failed, Review
- Drag pets from selector to desktop
- Click pet to trigger reactions
- Keyboard controls for selected pet
- Random idle animations

## Controls

| Input | Action |
|-------|--------|
| Left-click on pet | Cycle reaction (wave/jump) |
| Drag pet | Reposition on desktop |
| Arrow Left/Right | Cycle animation state |
| Arrow Up | Review state |
| Arrow Down | Failed state |
| Period (.) | Waiting state |
| Space | Jump |
| Escape | Exit |

## Build

```bash
# Using MinGW-w64
gcc -Wall -Wextra -O2 -mwindows -o build/codex-petdex-win.exe \
    src/main.c src/pet.c src/sprite.c src/animation.c src/window.c src/selector.c \
    -lole32 -lwindowscodecs -luuid

# Or use make
make

# Or use build.bat
build.bat
```

## Project Structure

```
codex-petdex-win/
├── src/
│   ├── main.c        - Entry point, message loop
│   ├── pet.c/h       - Pet loading and path resolution
│   ├── sprite.c/h    - WIC sprite sheet loading
│   ├── animation.c/h - Animation state machine
│   ├── window.c/h    - Layered window management
│   └── selector.c/h  - Pet selector UI and desktop pet logic
├── build/            - Build output
└── README.md
```

## Requirements

- Windows 10/11
- MinGW-w64 (for building)

## Usage

1. Visit https://petdex.crafter.run/ to download the sprite pack for your favorite character.
2. Extract the pack and place the folder into a `my` directory located in the same folder as the exe.
3. Run the exe, select your favorite character, and enjoy!
