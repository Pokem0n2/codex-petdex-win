#include "window.h"
#include "animation.h"
#include "sprite.h"
#include <string.h>
#define __USE_MINGW_ANSI_STDIO 1

HINSTANCE g_hinst = NULL;
Pet *g_pet = NULL;

static int g_current_pet_idx = 0;
static int g_pet_count = 0;
static PetInfo *g_pet_list = NULL;
static HWND g_hwnd = NULL;

// Drag tracking
static int g_is_dragging = 0;
static int g_drag_start_x = 0;
static int g_drag_start_y = 0;
static int g_window_start_x = 0;
static int g_window_start_y = 0;

static void build_sprite_path(wchar_t *out, const wchar_t *base, const char *pet_name, int is_png) {
    wchar_t wname[128];
    MultiByteToWideChar(CP_ACP, 0, pet_name, -1, wname, 128);
    if (is_png)
        wsprintfW(out, L"%ls/%ls/spritesheet.png", base, wname);
    else
        wsprintfW(out, L"%ls/%ls/spritesheet.webp", base, wname);
}

static void reload_pet_window(void) {
    if (!g_pet || !g_hwnd || !g_pet_list) return;

    const PetInfo *pi = &g_pet_list[g_current_pet_idx];
    const wchar_t *base = get_petdex_path_w();
    wchar_t path[MAX_PATH];
    build_sprite_path(path, base, pi->name, 0);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        build_sprite_path(path, base, pi->name, 1);
    }

    free_spritesheet(g_pet);
    memset(g_pet->states, 0, sizeof(g_pet->states));
    g_pet->current_state = STATE_IDLE;
    strncpy_s(g_pet->name, 64, pi->name, _TRUNCATE);

    if (!load_spritesheet(g_pet, path)) return;

    Frame *f = &g_pet->states[STATE_IDLE].frames[0];

    RECT wr = {0, 0, f->width, f->height};
    AdjustWindowRect(&wr, WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX, FALSE);
    int win_w = wr.right - wr.left;
    int win_h = wr.bottom - wr.top;

    RECT rc;
    GetWindowRect(g_hwnd, &rc);
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int new_x = rc.left;
    int new_y = rc.top;
    if (rc.left + win_w > screen_w) new_x = screen_w - win_w;
    if (rc.top + win_h > screen_h - 40) new_y = screen_h - win_h - 40;
    if (new_x < 0) new_x = 0;
    if (new_y < 0) new_y = 0;

    SetWindowPos(g_hwnd, 0, new_x, new_y, win_w, win_h, SWP_NOZORDER | SWP_SHOWWINDOW);
    render_frame(g_hwnd, g_pet);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == 1) {
                advance_animation(g_pet);
                render_frame(hwnd, g_pet);
            }
            return 0;

        case WM_LBUTTONDOWN: {
            // Start dragging from title bar area
            POINT pt = {LOWORD(lp), HIWORD(lp)};
            RECT wr;
            GetWindowRect(hwnd, &wr);
            // Check if in title bar area (top portion of window)
            int title_bar_height = (wr.bottom - wr.top) - 208; // 208 is the pet content height
            if (title_bar_height < 20) title_bar_height = 30; // minimum title bar

            // Convert client coords to screen for comparison
            POINT pt_screen = pt;
            ClientToScreen(hwnd, &pt_screen);

            if (pt_screen.y < wr.top + title_bar_height) {
                // In title bar area - start drag
                g_is_dragging = 1;
                g_drag_start_x = pt_screen.x;
                g_drag_start_y = pt_screen.y;
                g_window_start_x = wr.left;
                g_window_start_y = wr.top;
                SetCapture(hwnd);
            } else {
                // In pet content area - change animation state
                set_state(g_pet, (g_pet->current_state + 1) % STATE_COUNT);
                render_frame(hwnd, g_pet);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (g_is_dragging) {
                POINT pt;
                GetCursorPos(&pt);
                int new_x = g_window_start_x + (pt.x - g_drag_start_x);
                int new_y = g_window_start_y + (pt.y - g_drag_start_y);

                // Clamp to screen
                int screen_w = GetSystemMetrics(SM_CXSCREEN);
                int screen_h = GetSystemMetrics(SM_CYSCREEN);
                RECT rc;
                GetWindowRect(hwnd, &rc);
                int win_w = rc.right - rc.left;
                int win_h = rc.bottom - rc.top;
                if (new_x < 0) new_x = 0;
                if (new_x + win_w > screen_w) new_x = screen_w - win_w;
                if (new_y < 0) new_y = 0;
                if (new_y + win_h > screen_h) new_y = screen_h - win_h;

                SetWindowPos(hwnd, NULL, new_x, new_y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (g_is_dragging) {
                g_is_dragging = 0;
                ReleaseCapture();
            }
            return 0;
        }

        case WM_KEYDOWN: {
            int key = (int)wp;
            if (key >= '1' && key <= '9') {
                set_state(g_pet, key - '1');
                render_frame(hwnd, g_pet);
            }
            if (key == VK_LEFT || key == VK_RIGHT) {
                if (g_pet_count <= 1) return 0;
                int dir = (key == VK_RIGHT) ? 1 : -1;
                g_current_pet_idx = (g_current_pet_idx + dir + g_pet_count) % g_pet_count;
                reload_pet_window();
            }
            if (key == VK_ESCAPE) {
                ShowWindow(hwnd, SW_HIDE);
            }
            return 0;
        }

        case WM_DESTROY:
            KillTimer(hwnd, 1);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

HWND create_layered_window(Pet *pet, PetInfo *pet_list, int pet_count, int initial_idx) {
    g_pet_list = pet_list;
    g_pet_count = pet_count;
    g_current_pet_idx = initial_idx;
    g_pet = pet;
    g_hwnd = NULL;
    g_is_dragging = 0;

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = g_hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"DigitPetClass";
    if (!RegisterClassExW(&wc)) return NULL;

    Frame *f = &pet->states[STATE_IDLE].frames[0];
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);

    RECT wr = {0, 0, f->width, f->height};
    AdjustWindowRect(&wr, WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX, FALSE);
    int win_w = wr.right - wr.left;
    int win_h = wr.bottom - wr.top;

    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        L"DigitPetClass", L"Digit Pet",
        WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX,
        screen_w - win_w, screen_h - win_h - 40,
        win_w, win_h,
        NULL, NULL, g_hinst, NULL
    );

    if (hwnd) {
        g_hwnd = hwnd;
        SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
        SetTimer(hwnd, 1, 83, NULL);
    }

    return hwnd;
}

void render_frame(HWND hwnd, Pet *pet) {
    AnimState *state = &pet->states[pet->current_state];
    if (state->count == 0) return;

    Frame *f = &state->frames[state->current];
    int fw = f->width;
    int fh = f->height;

    HDC hdc = GetDC(hwnd);
    HDC hdcMem = CreateCompatibleDC(hdc);
    HBITMAP hbmp = NULL;
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    void *ppvBits = NULL;

    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = fw;
    bmi.bmiHeader.biHeight = -fh;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    hbmp = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &ppvBits, NULL, 0);
    if (!hbmp) { DeleteDC(hdcMem); ReleaseDC(hwnd, hdc); return; }

    SelectObject(hdcMem, hbmp);

    int row;
    int row_h = 208;
    BYTE *src = pet->pixels + (pet->current_state * row_h * pet->sheet_w + f->x) * 4;
    BYTE *dst = (BYTE*)ppvBits;
    for (row = 0; row < fh; row++) {
        memcpy(dst + row * fw * 4, src + row * pet->sheet_w * 4, fw * 4);
    }

    RECT rc;
    GetWindowRect(hwnd, &rc);
    POINT ptDst;
    ptDst.x = rc.left;
    ptDst.y = rc.top;
    SIZE sz;
    sz.cx = fw;
    sz.cy = fh;
    BLENDFUNCTION blend;
    blend.BlendOp = AC_SRC_OVER;
    blend.BlendFlags = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    POINT ptSrc;
    ptSrc.x = 0;
    ptSrc.y = 0;

    UpdateLayeredWindow(hwnd, hdc, &ptDst, &sz, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    DeleteObject(hbmp);
    DeleteDC(hdcMem);
    ReleaseDC(hwnd, hdc);
}

void cleanup_window(HWND hwnd, Pet *pet) {
    if (hwnd) { KillTimer(hwnd, 1); DestroyWindow(hwnd); }
    if (pet) free_spritesheet(pet);
    g_pet = NULL;
    g_pet_list = NULL;
    g_pet_count = 0;
    g_hwnd = NULL;
}