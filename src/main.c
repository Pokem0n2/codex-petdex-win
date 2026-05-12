#include "window.h"
#include "sprite.h"
#include "selector.h"
#include <stdlib.h>
#include <string.h>
#define __USE_MINGW_ANSI_STDIO 1

extern Pet *g_pet;

static void build_sprite_path(wchar_t *out, const wchar_t *base, const char *pet_name, int is_png) {
    wchar_t wname[128];
    MultiByteToWideChar(CP_ACP, 0, pet_name, -1, wname, 128);
    if (is_png)
        wsprintfW(out, L"%ls/%ls/spritesheet.png", base, wname);
    else
        wsprintfW(out, L"%ls/%ls/spritesheet.webp", base, wname);
}

int WINAPI WinMain(HINSTANCE hinst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;
    g_hinst = hinst;

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) return 1;

    PetSelector *sel = create_pet_selector();
    if (!sel || sel->pet_count == 0) {
        MessageBoxW(NULL, L"No pets found in petdex.", L"Error", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    int selected = run_pet_selector(sel);
    destroy_pet_selector(sel);

    if (selected < 1) {
        CoUninitialize();
        return 0;
    }

    PetInfo *pets = NULL;
    int pet_count = get_all_pets(&pets);
    if (!pets || pet_count == 0) {
        CoUninitialize();
        return 1;
    }

    int idx = selected - 1;
    if (idx < 0 || idx >= pet_count) idx = 0;

    const PetInfo *pi = &pets[idx];
    const wchar_t *base = get_petdex_path_w();
    wchar_t sprite_path[MAX_PATH];
    build_sprite_path(sprite_path, base, pi->name, 0);
    if (GetFileAttributesW(sprite_path) == INVALID_FILE_ATTRIBUTES) {
        build_sprite_path(sprite_path, base, pi->name, 1);
    }

    Pet *pet = calloc(1, sizeof(Pet));
    strncpy_s(pet->name, 64, pi->name, _TRUNCATE);

    if (!load_spritesheet(pet, sprite_path)) {
        MessageBoxW(NULL, L"Failed to load spritesheet.", L"Error", MB_ICONERROR);
        free(pet);
        free_pet_list(pets);
        CoUninitialize();
        return 1;
    }

    HWND hwnd = create_layered_window(pet, pets, pet_count, idx);
    if (!hwnd) {
        free_spritesheet(pet);
        free(pet);
        free_pet_list(pets);
        CoUninitialize();
        return 1;
    }

    ShowWindow(hwnd, SW_SHOW);
    render_frame(hwnd, pet);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    cleanup_window(hwnd, pet);
    free(pet);
    free_pet_list(pets);
    CoUninitialize();
    return 0;
}
