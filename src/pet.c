#include "pet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ShlObj.h>
#define __USE_MINGW_ANSI_STDIO 1

static wchar_t g_petdex_path[MAX_PATH] = {0};

const wchar_t *get_petdex_path_w(void) {
    if (g_petdex_path[0] != 0) return g_petdex_path;
    if (GetEnvironmentVariableW(L"DIGIT_PET_PATH", g_petdex_path, MAX_PATH) > 0 && g_petdex_path[0] != L'\0')
        return g_petdex_path;
    // Use path relative to exe: get exe dir, append "my/petdex"
    wchar_t exe_path[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, exe_path, MAX_PATH);
    // Remove exe filename to get directory
    wchar_t *last_bslash = wcsrchr(exe_path, L'\\');
    if (last_bslash) *last_bslash = L'\0';
    wcsncpy_s(g_petdex_path, MAX_PATH, exe_path, _TRUNCATE);
    wcscat_s(g_petdex_path, MAX_PATH, L"\\my");
    return g_petdex_path;
}

const char *state_name(int state) {
    static const char *names[STATE_COUNT] = {
        "Idle", "Run Right", "Run Left", "Waving",
        "Jumping", "Failed", "Waiting", "Running", "Review"
    };
    if (state < 0 || state >= STATE_COUNT) return "Unknown";
    return names[state];
}

static void trim_dot_folder(char *out, const wchar_t *src, size_t out_size) {
    WideCharToMultiByte(CP_UTF8, 0, src, -1, out, (int)out_size, NULL, NULL);
}

int get_all_pets(PetInfo **out_pets) {
    const wchar_t *base = get_petdex_path_w();

    WIN32_FIND_DATAW fd;
    wchar_t search_path[MAX_PATH];
    swprintf(search_path, MAX_PATH, L"%ls\\*", base);

    int capacity = 64;
    int count = 0;
    PetInfo *pets = calloc(capacity, sizeof(PetInfo));

    HANDLE h = FindFirstFileW(search_path, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        *out_pets = pets;
        return 0;
    }

    while (FindNextFileW(h, &fd)) {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        wchar_t webp[MAX_PATH], png[MAX_PATH];
        swprintf(webp, MAX_PATH, L"%ls\\%ls\\spritesheet.webp", base, fd.cFileName);
        swprintf(png, MAX_PATH, L"%ls\\%ls\\spritesheet.png", base, fd.cFileName);

        if (GetFileAttributesW(webp) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(png) == INVALID_FILE_ATTRIBUTES)
            continue;

        if (count >= capacity) {
            capacity *= 2;
            pets = realloc(pets, capacity * sizeof(PetInfo));
        }

        trim_dot_folder(pets[count].name, fd.cFileName, 64);

        wchar_t json_path[MAX_PATH];
        swprintf(json_path, MAX_PATH, L"%ls\\%ls\\pet.json", base, fd.cFileName);

        pets[count].display_name[0] = '\0';
        FILE *f = _wfopen(json_path, L"r, ccs=UTF-8");
        if (f) {
            char line[256] = {0};
            while (fgets(line, sizeof(line), f)) {
                if (strstr(line, "\"displayName\"")) {
                    char *p = strchr(line, ':');
                    if (p) {
                        p++;
                        while (*p == ' ' || *p == '"') p++;
                        char *end = strrchr(p, '"');
                        if (end) *end = '\0';
                        strncpy_s(pets[count].display_name, 64, p, _TRUNCATE);
                    }
                    break;
                }
            }
            fclose(f);
        }

        if (!pets[count].display_name[0]) {
            strncpy_s(pets[count].display_name, 64, pets[count].name, _TRUNCATE);
        }

        count++;
    }

    FindClose(h);
    *out_pets = pets;
    return count;
}

void free_pet_list(PetInfo *pets) {
    free(pets);
}
