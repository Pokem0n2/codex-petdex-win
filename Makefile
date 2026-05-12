CC = gcc
CFLAGS = -Wall -Wextra -O2 -mwindows
LDFLAGS = -lole32 -lwindowscodecs -luuid
SRCS = src/main.c src/pet.c src/sprite.c src/animation.c src/window.c src/selector.c
TARGET = build/codex-petdex-win.exe

all:
	mkdir -p build
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) $(LDFLAGS)

clean:
	rm -rf build

.PHONY: all clean
