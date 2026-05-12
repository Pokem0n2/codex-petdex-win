#include "selector.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <wincodec.h>
#include <stdio.h>
#define __USE_MINGW_ANSI_STDIO 1

// Debug file logging (disabled)
#define debug_log(...) ((void)0)

extern HINSTANCE g_hinst;
extern const wchar_t *get_petdex_path_w(void);
extern IWICImagingFactory *g_wic_factory;

// Forward declarations
#define MAX_DESKTOP_PETS 16
typedef struct {
    HWND hwnd;
    int pet_idx;
    int anim_state;
    int frame;
    DWORD last_frame_time;
    int x, y;
    int orig_x, orig_y;
    int dragging;
    int drag_start_x, drag_start_y;
    int last_drag_x, last_drag_y;
    DWORD reaction_end_time;
    DWORD last_drag_frame_time;
    DWORD last_activity_time;
    int jump_state;
    int jump_velocity;
    int jump_start_y;
    int move_state;
    int path_type;
    float path_t;
    float path_dx, path_dy;
    float speed;
    float center_x, center_y;
    float radius_x, radius_y;
    float angle, angle_speed;
    float start_x, start_y;
    float end_x, end_y;
} DesktopPet;

static DesktopPet g_desktop_pets[MAX_DESKTOP_PETS];
static int g_desktop_pet_count = 0;

// Global keyboard hook for ESC to exit
static HHOOK g_kbd_hook = NULL;
static int g_exit_requested = 0;

static LRESULT CALLBACK keyboard_hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code >= 0 && wparam == VK_ESCAPE && !g_exit_requested) {
        g_exit_requested = 1;
        for (int i = 0; i < g_desktop_pet_count; i++) {
            PostMessageW(g_desktop_pets[i].hwnd, WM_CLOSE, 0, 0);
        }
    }
    return CallNextHookEx(g_kbd_hook, code, wparam, lparam);
}

static void install_keyboard_hook(void) {
    if (!g_kbd_hook) {
        g_kbd_hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_hook_proc, GetModuleHandle(NULL), 0);
    }
}

static void uninstall_keyboard_hook(void) {
    if (g_kbd_hook) {
        UnhookWindowsHookEx(g_kbd_hook);
        g_kbd_hook = NULL;
    }
}

static PetSelector *g_sel = NULL;
static int g_exit_code = -1;
static int g_preview_frame = 0;
static int g_anim_state = 0;  // Current animation state (row): 0=Idle, 1=Run Right, etc.

// Frame counts per animation state (Codex pet-states.ts)
static const int FRAME_COUNTS[] = {6, 8, 8, 4, 5, 8, 6, 6, 6};
// Per-frame duration in ms (total duration / frame count)
static const int FRAME_DURATIONS[] = {183, 133, 133, 175, 168, 153, 168, 137, 172};

// Idle cycle pattern (Codex pet-floater)
static const int IDLE_CYCLE[] = {0, 0, 0, 0, 6, 3, 4, 8};
static const int IDLE_CYCLE_COUNT = 8;

// Idle cycle timing (1700-3000ms random per Codex)
static const int IDLE_TICK_MIN_MS = 1700;
static const int IDLE_TICK_MAX_MS = 3000;

// Click reaction timing (~1100ms per Codex)
static const int REACTION_MS = 1100;
// Drag release cooldown (~600ms per Codex)
static const int RUN_TAIL_MS = 600;

// Animation state machine
typedef enum {
    STATE_IDLE_CYCLE,  // Normal idle cycling
    STATE_REACTION,     // Click-triggered waving/jumping
    STATE_DRAGGING,     // Being dragged - running left/right
    STATE_COOLDOWN      // Post-drag cooldown before returning to idle cycle
} PreviewState;

// Animation timing
static DWORD g_last_frame_time = 0;
static DWORD g_last_state_time = 0;
static int g_idle_cycle_idx = 0;
static PreviewState g_preview_state = STATE_IDLE_CYCLE;
static int g_reaction_toggle = 0;  // Alternates between waving/jumping
static int g_last_drag_x = 0;      // For tracking drag direction

// Cached spritesheet data to avoid reloading on every frame
static BYTE *g_cached_pixels = NULL;
static int g_cached_w = 0;
static int g_cached_h = 0;
static int g_cached_pet_idx = -1;
static int g_cached_anim_state = -1;
static wchar_t g_cached_path[MAX_PATH] = {0};

// Drag state
static int g_dragging = 0;
static HWND g_drag_window = NULL;
static int g_drag_x = 0;
static int g_drag_y = 0;
static int g_click_start_x = 0;
static int g_click_start_y = 0;
static int g_moved = 0;  // Track if significant movement occurred
static int g_drag_pet_idx = -1;
static BYTE *g_drag_pixels = NULL;
static int g_drag_sheet_w = 0;
static int g_drag_sheet_h = 0;

static wchar_t *utf8_to_wide(const char *utf8) {
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (len == 0) return NULL;
    wchar_t *out = calloc(len, sizeof(wchar_t));
    if (out) MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, len);
    return out;
}

static const int FRAME_W = 192;
static const int FRAME_H = 208;
static const int DRAG_W = 48;   // 1/4 of 192
static const int DRAG_H = 52;    // 1/4 of 208

// Desktop pet tracking
#define MAX_DESKTOP_PETS 16
static int g_selected_pet_id = -1;  // -1 = none selected
static ATOM g_pet_atom = 0;

// Register window class for desktop pets
static void register_pet_class(void) {
    if (g_pet_atom) return;
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = g_hinst;
    wc.lpszClassName = L"DesktopPetWindow";
    g_pet_atom = RegisterClassExW(&wc);
}

// Forward declarations
static void build_sprite_path(wchar_t *out, const wchar_t *base, const char *pet_name, int is_png);
static void update_drag(int x, int y);

// Pet window class for desktop pets - use built-in STATIC class
static BYTE *g_pet_pixels[MAX_DESKTOP_PETS] = {0};
static int g_pet_sheet_w[MAX_DESKTOP_PETS] = {0};
static int g_pet_sheet_h[MAX_DESKTOP_PETS] = {0};

// Forward function to get spritesheet for drag
static int load_spritesheet_for_drag(int pet_idx, BYTE **out_pixels, int *out_w, int *out_h);

static LRESULT CALLBACK pet_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    int pet_id = -1;
    for (int i = 0; i < g_desktop_pet_count; i++) {
        if (g_desktop_pets[i].hwnd == hwnd) {
            pet_id = i;
            break;
        }
    }
    if (pet_id < 0) return DefWindowProcW(hwnd, msg, wp, lp);

    DesktopPet *pet = &g_desktop_pets[pet_id];
    BYTE *pixels = g_pet_pixels[pet_id];

    switch (msg) {
        case WM_CREATE: {
            char buf[256];
            snprintf(buf, sizeof(buf), "WM_CREATE: hwnd=%p", (void*)hwnd);
            debug_log("%s", buf);
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_SHOWWINDOW: {
            char buf[256];
            snprintf(buf, sizeof(buf), "WM_SHOWWINDOW: hwnd=%p, show=%d", (void*)hwnd, (int)wp);
            debug_log("%s", buf);
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);  // Acknowledge the paint request

            {
                char buf[256];
                snprintf(buf, sizeof(buf), "WM_PAINT: pet_id=%d", pet_id);
                debug_log("%s", buf);
            }

            // Get screen DC for UpdateLayeredWindow
            HDC hdcScreen = GetDC(NULL);

            // Render current frame to a memory DC
            HDC hdcMem = CreateCompatibleDC(hdcScreen);

            BITMAPINFO bmi = {0};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = DRAG_W;
            bmi.bmiHeader.biHeight = -DRAG_H;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;

            void *bits = NULL;
            HBITMAP hbmp = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
            if (bits && pixels) {
                // Extract frame (simple loop) - convert to premultiplied alpha for UpdateLayeredWindow
                int n = FRAME_COUNTS[pet->anim_state];
                int frame_idx = pet->frame % n;
                int fx = frame_idx * FRAME_W;
                int src_row_y = pet->anim_state * FRAME_H;
                BYTE *dst = (BYTE*)bits;

                for (int row = 0; row < DRAG_H; row++) {
                    int src_row = src_row_y + row * 4;  // 208/52 = 4
                    for (int col = 0; col < DRAG_W; col++) {
                        int src_col = col * 4;  // 192/48 = 4
                        BYTE *src_pixel = pixels + (src_row * g_pet_sheet_w[pet_id] + fx + src_col) * 4;
                        BYTE *dst_pixel = dst + (row * DRAG_W + col) * 4;

                        // WIC gives BGRA, need premultiplied alpha for AC_SRC_ALPHA
                        int b = src_pixel[0];
                        int g = src_pixel[1];
                        int r = src_pixel[2];
                        int a = src_pixel[3];

                        if (a > 64) {
                            // Premultiply RGB by alpha
                            dst_pixel[0] = (BYTE)((b * a) / 255);
                            dst_pixel[1] = (BYTE)((g * a) / 255);
                            dst_pixel[2] = (BYTE)((r * a) / 255);
                            dst_pixel[3] = (BYTE)a;
                        } else {
                            // Fully transparent - alpha=0 so desktop shows through
                            dst_pixel[0] = 0;
                            dst_pixel[1] = 0;
                            dst_pixel[2] = 0;
                            dst_pixel[3] = 0;
                        }
                    }
                }

                SelectObject(hdcMem, hbmp);

                POINT ptSrc = {0, 0};
                POINT ptDst = {pet->x, pet->y};
                SIZE size = {DRAG_W, DRAG_H};
                BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

                UpdateLayeredWindow(hwnd, hdcScreen, &ptDst, &size, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
            }

            if (hbmp) DeleteObject(hbmp);
            DeleteDC(hdcMem);
            ReleaseDC(NULL, hdcScreen);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_TIMER:
            if (wp == 1) {
                DWORD now = GetTickCount();

                // Only handle keyboard for selected pet that is not dragging
                if (pet_id == g_selected_pet_id && !pet->dragging) {
                    if (GetAsyncKeyState(VK_LEFT) & 0x8000) {
                        pet->anim_state = (pet->anim_state - 1 + 9) % 9;
                        pet->frame = 0;
                        pet->last_activity_time = now;
                        pet->move_state = 0;
                    }
                    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) {
                        pet->anim_state = (pet->anim_state + 1) % 9;
                        pet->frame = 0;
                        pet->last_activity_time = now;
                        pet->move_state = 0;
                    }
                    if (GetAsyncKeyState(VK_SPACE) & 0x8000) {
                        if (!pet->jump_state) {
                            pet->jump_state = 1;
                            pet->jump_velocity = 12;
                            pet->jump_start_y = pet->y;
                            pet->anim_state = 4;
                            pet->frame = 0;
                        }
                        pet->last_activity_time = now;
                        pet->move_state = 0;
                    }
                    if (GetAsyncKeyState(VK_UP) & 0x8000) {
                        pet->anim_state = 8;
                        pet->frame = 0;
                        pet->reaction_end_time = now + REACTION_MS;
                        pet->last_activity_time = now;
                        pet->move_state = 0;
                    }
                    if (GetAsyncKeyState(VK_OEM_PERIOD) & 0x8000) {
                        pet->anim_state = 6;
                        pet->frame = 0;
                        pet->reaction_end_time = now + REACTION_MS;
                        pet->last_activity_time = now;
                        pet->move_state = 0;
                    }
                    if (GetAsyncKeyState(VK_DOWN) & 0x8000) {
                        pet->anim_state = 5;
                        pet->frame = 0;
                        pet->reaction_end_time = now + REACTION_MS;
                        pet->last_activity_time = now;
                        pet->move_state = 0;
                    }
                }

                // Handle jump physics
                if (pet->jump_state == 1) {
                    pet->y -= pet->jump_velocity;
                    pet->jump_velocity -= 1;
                    if (pet->jump_velocity <= 0) {
                        pet->jump_state = 2;
                        pet->jump_velocity = 0;
                    }
                    SetWindowPos(hwnd, NULL, pet->x, pet->y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
                } else if (pet->jump_state == 2) {
                    pet->jump_velocity += 1;
                    pet->y += pet->jump_velocity;
                    if (pet->y >= pet->jump_start_y) {
                        pet->y = pet->jump_start_y;
                        pet->jump_state = 0;
                        pet->anim_state = 0;
                    }
                    SetWindowPos(hwnd, NULL, pet->x, pet->y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
                }

                // ESC to exit - check in timer
                if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                    for (int i = 0; i < g_desktop_pet_count; i++) {
                        DestroyWindow(g_desktop_pets[i].hwnd);
                    }
                    g_desktop_pet_count = 0;
                    uninstall_keyboard_hook();
                    PostQuitMessage(0);
                    return 0;
                }

                // Random idle activity for ALL pets (not dragging, not jumping, 5-10s idle)
                if (!pet->dragging && !pet->jump_state && pet->move_state == 0) {
                    if (now - pet->last_activity_time > (DWORD)(5000 + (rand() % 5000))) {
                        int screen_w = GetSystemMetrics(SM_CXSCREEN);
                        int screen_h = GetSystemMetrics(SM_CYSCREEN);
                        int max_x = screen_w - DRAG_W;
                        int max_y = screen_h - DRAG_H;
                        int rand_type = rand() % 100;

                        if (rand_type < 15) {
                            // Line movement in random direction
                            float angle = (rand() % 360) * 3.14159f / 180.0f;
                            float dist = 60 + rand() % 120;
                            pet->end_x = pet->x + (int)(dist * cosf(angle));
                            pet->end_y = pet->y + (int)(dist * sinf(angle));
                            // Clamp to screen
                            if (pet->end_x < 0) pet->end_x = 0;
                            if (pet->end_x > max_x) pet->end_x = max_x;
                            if (pet->end_y < 0) pet->end_y = 0;
                            if (pet->end_y > max_y) pet->end_y = max_y;
                            pet->start_x = pet->x;
                            pet->start_y = pet->y;
                            pet->path_type = 0;
                            pet->path_t = 0;
                            pet->speed = 1.5f + (rand() % 2);
                            pet->move_state = 1;
                            pet->anim_state = 7;  // running
                            pet->frame = 0;
                        } else if (rand_type < 25) {
                            // Circle/ellipse movement
                            pet->center_x = pet->x;
                            pet->center_y = pet->y;
                            pet->radius_x = 25 + rand() % 40;
                            pet->radius_y = pet->radius_x * (0.4f + (rand() % 60) / 100.0f);
                            pet->angle = 0;
                            pet->angle_speed = (0.015f + (rand() % 15) / 1000.0f) * ((rand() % 2) ? 1 : -1);
                            pet->path_type = 1;
                            pet->path_t = 0;
                            pet->start_x = pet->x;
                            pet->start_y = pet->y;
                            pet->move_state = 1;
                            pet->anim_state = 7;
                            pet->frame = 0;
                        } else if (rand_type < 35) {
                            // Jumping (1-3 small hops)
                            int hops = 1 + rand() % 3;
                            pet->jump_state = 1;
                            pet->jump_velocity = 6 + rand() % 4;
                            pet->jump_start_y = pet->y;
                            pet->anim_state = 4;  // jumping
                            pet->frame = 0;
                            pet->reaction_end_time = now + hops * 300;
                        } else if (rand_type < 45) {
                            // Review animation (10-20 seconds)
                            pet->anim_state = 8;
                            pet->frame = 0;
                            pet->reaction_end_time = now + 10000 + rand() % 10000;
                        } else if (rand_type < 55) {
                            // Waiting animation (10-20 seconds)
                            pet->anim_state = 6;
                            pet->frame = 0;
                            pet->reaction_end_time = now + 10000 + rand() % 10000;
                        } else if (rand_type < 65) {
                            // Failed animation
                            pet->anim_state = 5;
                            pet->frame = 0;
                            pet->reaction_end_time = now + 500;
                        } else if (rand_type < 75) {
                            // Idle for a while
                            pet->anim_state = 0;  // idle
                            pet->frame = 0;
                            pet->reaction_end_time = now + 5000;
                        }
                        // Some actions don't set reaction_end_time, so set last_activity_time to now for them
                        if (rand_type >= 45) {
                            pet->last_activity_time = now;
                        }
                    }
                }

                // Handle movement along path
                if (pet->move_state == 1) {
                    if (pet->path_type == 0) {
                        // Line movement
                        pet->path_t += pet->speed / 100.0f;
                        if (pet->path_t >= 1.0f) {
                            pet->path_t = 1.0f;
                            pet->x = (int)pet->end_x;
                            pet->y = (int)pet->end_y;
                            pet->move_state = 0;
                            pet->anim_state = 0;
                        } else {
                            pet->x = (int)(pet->start_x + (pet->end_x - pet->start_x) * pet->path_t);
                            pet->y = (int)(pet->start_y + (pet->end_y - pet->start_y) * pet->path_t);
                        }
                        // Determine animation based on horizontal direction
                        float dx = pet->end_x - pet->start_x;
                        if (fabsf(dx) > 5) {
                            pet->anim_state = (dx < 0) ? 2 : 1;  // running-left or running-right
                        }
                    } else if (pet->path_type == 1) {
                        // Circle/ellipse movement
                        float prev_x = pet->x;
                        pet->angle += pet->angle_speed;
                        pet->path_t += fabsf(pet->angle_speed);
                        if (pet->path_t >= 6.28f) {
                            // Completed circle - stay at final position
                            pet->x = (int)(pet->center_x + pet->radius_x * cosf(pet->angle));
                            pet->y = (int)(pet->center_y + pet->radius_y * sinf(pet->angle));
                            pet->move_state = 0;
                            pet->anim_state = 0;
                        } else {
                            pet->x = (int)(pet->center_x + pet->radius_x * cosf(pet->angle));
                            pet->y = (int)(pet->center_y + pet->radius_y * sinf(pet->angle));
                        }
                        // Determine animation based on horizontal direction
                        float dx = pet->x - prev_x;
                        if (fabsf(dx) > 0.5f) {
                            pet->anim_state = (dx < 0) ? 2 : 1;  // running-left or running-right
                        }
                    }

                    // Clamp to screen boundaries
                    int screen_w = GetSystemMetrics(SM_CXSCREEN);
                    int screen_h = GetSystemMetrics(SM_CYSCREEN);
                    if (pet->x < 0) pet->x = 0;
                    if (pet->x > screen_w - DRAG_W) pet->x = screen_w - DRAG_W;
                    if (pet->y < 0) pet->y = 0;
                    if (pet->y > screen_h - DRAG_H) pet->y = screen_h - DRAG_H;

                    SetWindowPos(hwnd, NULL, pet->x, pet->y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
                }

                // Check if reaction is over (not during movement or jumping)
                if (pet->reaction_end_time && now >= pet->reaction_end_time && !pet->jump_state && pet->move_state == 0) {
                    pet->reaction_end_time = 0;
                    pet->anim_state = 0;
                    pet->last_activity_time = now;
                }

                // Advance animation frame
                int frame_duration = FRAME_DURATIONS[pet->anim_state];
                if (now - pet->last_frame_time >= (DWORD)frame_duration) {
                    pet->frame++;
                    pet->last_frame_time = now;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            return 0;

        case WM_LBUTTONDOWN: {
            // Get screen coordinates for drag start
            POINT pt;
            GetCursorPos(&pt);
            pet->dragging = 1;
            pet->drag_start_x = pt.x;
            pet->drag_start_y = pt.y;
            pet->last_drag_x = pt.x;
            pet->last_drag_y = pt.y;
            pet->orig_x = pet->x;
            pet->orig_y = pet->y;
            SetCapture(hwnd);
            g_selected_pet_id = pet_id;
            debug_log("pet WM_LBUTTONDOWN: id=%d selected", pet_id);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (pet->dragging) {
                // Use GetMessagePos for reliable position during mouse capture
                DWORD msg_pos = GetMessagePos();
                int msg_x = (int)(short)LOWORD(msg_pos);
                int msg_y = (int)(short)HIWORD(msg_pos);

                // Calculate movement delta from last position
                int move_x = msg_x - pet->last_drag_x;
                int move_y = msg_y - pet->last_drag_y;
                (void)move_y;  // reserved for future vertical movement detection

                // Update pet position based on total drag distance from start
                int total_dx = msg_x - pet->drag_start_x;
                int total_dy = msg_y - pet->drag_start_y;
                pet->x = pet->orig_x + total_dx;
                pet->y = pet->orig_y + total_dy;

                // Clamp to screen boundaries
                int screen_w = GetSystemMetrics(SM_CXSCREEN);
                int screen_h = GetSystemMetrics(SM_CYSCREEN);
                if (pet->x < 0) pet->x = 0;
                if (pet->x > screen_w - DRAG_W) pet->x = screen_w - DRAG_W;
                if (pet->y < 0) pet->y = 0;
                if (pet->y > screen_h - DRAG_H) pet->y = screen_h - DRAG_H;

                // Direction-aware animation based on immediate movement delta
                // Use 1px threshold for responsive direction changes
                if (move_x > 1) {
                    pet->anim_state = 1;  // running-right
                    pet->frame = 0;       // reset frame on direction change
                } else if (move_x < -1) {
                    pet->anim_state = 2;  // running-left
                    pet->frame = 0;       // reset frame on direction change
                }

                // Update last drag position for next frame calculation
                pet->last_drag_x = msg_x;
                pet->last_drag_y = msg_y;

                // Advance animation frame based on time
                DWORD now = GetTickCount();
                int frame_duration = FRAME_DURATIONS[pet->anim_state];
                if (now - pet->last_drag_frame_time >= (DWORD)frame_duration) {
                    pet->frame++;
                    pet->last_drag_frame_time = now;
                }

                // Update window position and force redraw
                SetWindowPos(hwnd, NULL, pet->x, pet->y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (pet->dragging) {
                pet->dragging = 0;
                ReleaseCapture();
                // Check if it was a click (no significant movement)
                POINT pt;
                GetCursorPos(&pt);
                int dx = abs(pt.x - pet->drag_start_x);
                int dy = abs(pt.y - pet->drag_start_y);
                debug_log("pet WM_LBUTTONUP: id=%d, dx=%d dy=%d", pet_id, dx, dy);
                if (dx < 4 && dy < 4) {
                    // It was a click - trigger reaction
                    static int toggle = 0;
                    pet->anim_state = toggle ? 4 : 3;  // jumping or waving
                    pet->frame = 0;
                    pet->reaction_end_time = GetTickCount() + REACTION_MS;
                    pet->jump_state = 0;  // cancel any jump
                    pet->move_state = 0;  // cancel any movement
                    pet->last_activity_time = GetTickCount();
                    toggle = !toggle;
                    debug_log("pet click reaction: id=%d, state=%d", pet_id, pet->anim_state);
                } else {
                    // Was a drag - reset to idle
                    pet->anim_state = 0;  // idle
                    pet->frame = 0;
                    pet->move_state = 0;
                    pet->jump_state = 0;
                    pet->last_activity_time = GetTickCount();
                }
            }
            return 0;
        }

        case WM_DESTROY: {
            // Remove from tracking
            int pet_id_to_remove = -1;
            for (int i = 0; i < g_desktop_pet_count; i++) {
                if (g_desktop_pets[i].hwnd == hwnd) {
                    pet_id_to_remove = i;
                    break;
                }
            }
            if (pet_id_to_remove >= 0) {
                if (g_pet_pixels[pet_id_to_remove]) {
                    free(g_pet_pixels[pet_id_to_remove]);
                    g_pet_pixels[pet_id_to_remove] = NULL;
                }
                // Swap with last and decrement
                if (pet_id_to_remove < g_desktop_pet_count - 1) {
                    g_desktop_pets[pet_id_to_remove] = g_desktop_pets[g_desktop_pet_count - 1];
                    g_pet_pixels[pet_id_to_remove] = g_pet_pixels[g_desktop_pet_count - 1];
                    g_pet_pixels[g_desktop_pet_count - 1] = NULL;
                }
                g_desktop_pet_count--;
            }
            KillTimer(hwnd, 1);

            // If exit was requested and this was the last pet, quit the app
            if (g_exit_requested && g_desktop_pet_count == 0) {
                uninstall_keyboard_hook();
                PostQuitMessage(0);
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Load spritesheet for a specific pet (used for drag and desktop pets)
static int load_spritesheet_for_drag(int pet_idx, BYTE **out_pixels, int *out_w, int *out_h) {
    if (!g_sel || pet_idx < 0 || pet_idx >= g_sel->pet_count) return 0;

    wchar_t spath[MAX_PATH];
    const wchar_t *base = get_petdex_path_w();
    build_sprite_path(spath, base, g_sel->pets[pet_idx].name, 0);
    if (GetFileAttributesW(spath) == INVALID_FILE_ATTRIBUTES) {
        build_sprite_path(spath, base, g_sel->pets[pet_idx].name, 1);
    }

    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;
    UINT sheet_w = 0, sheet_h = 0;
    BYTE *pixels = NULL;

    if (!g_wic_factory) {
        CoCreateInstance(&CLSID_WICImagingFactory, NULL,
            CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void**)&g_wic_factory);
    }
    if (g_wic_factory) {
        HRESULT hr = g_wic_factory->lpVtbl->CreateDecoderFromFilename(
            g_wic_factory, spath, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
        if (SUCCEEDED(hr)) {
            hr = decoder->lpVtbl->GetFrame(decoder, 0, &frame);
            if (SUCCEEDED(hr)) {
                hr = frame->lpVtbl->GetSize(frame, &sheet_w, &sheet_h);
                if (SUCCEEDED(hr)) {
                    hr = g_wic_factory->lpVtbl->CreateFormatConverter(g_wic_factory, &converter);
                    if (SUCCEEDED(hr)) {
                        hr = converter->lpVtbl->Initialize(converter, (IWICBitmapSource*)frame,
                            &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0,
                            WICBitmapPaletteTypeCustom);
                        if (SUCCEEDED(hr)) {
                            pixels = calloc(sheet_w * sheet_h, 4);
                            if (pixels) {
                                converter->lpVtbl->CopyPixels(converter, NULL, sheet_w * 4, sheet_w * sheet_h * 4, pixels);
                            }
                        }
                    }
                }
            }
        }
        if (converter) converter->lpVtbl->Release(converter);
        if (frame) frame->lpVtbl->Release(frame);
        if (decoder) decoder->lpVtbl->Release(decoder);
    }

    if (!pixels) return 0;

    *out_pixels = pixels;
    *out_w = (int)sheet_w;
    *out_h = (int)sheet_h;
    return 1;
}

// Drag window: create/update/destroy
static void start_drag(int x, int y) {
    if (g_dragging) return;

    // Load spritesheet for current pet
    int pet_idx = g_sel ? g_sel->selected_index : 0;
    if (!load_spritesheet_for_drag(pet_idx, &g_drag_pixels, &g_drag_sheet_w, &g_drag_sheet_h)) {
        debug_log("start_drag: failed to load spritesheet");
        return;
    }
    g_drag_pet_idx = pet_idx;

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "start_drag: pet_idx=%d, pos=(%d,%d), sheet=%dx%d",
                 pet_idx, x, y, g_drag_sheet_w, g_drag_sheet_h);
        debug_log("%s", buf);
    }

    g_dragging = 1;
    g_drag_x = x;
    g_drag_y = y;

    // Create a layered popup window
    g_drag_window = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        L"STATIC", L"",
        WS_POPUP,
        x, y, DRAG_W, DRAG_H,
        NULL, NULL, g_hinst, NULL);

    if (g_drag_window) {
        ShowWindow(g_drag_window, SW_SHOW);
        update_drag(x, y);
    }
}

static void update_drag(int x, int y) {
    if (!g_dragging || !g_drag_window) return;

    g_drag_x = x;
    g_drag_y = y;

    // Render current frame to a memory DC (scaled to DRAG_W x DRAG_H)
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = DRAG_W;
    bmi.bmiHeader.biHeight = -DRAG_H;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HBITMAP hbmp = CreateDIBSection(hdcMem, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hbmp || !bits) {
        DeleteDC(hdcMem);
        ReleaseDC(NULL, hdcScreen);
        return;
    }

    // Fill with background
    RECT rc = {0, 0, DRAG_W, DRAG_H};
    HBRUSH br = CreateSolidBrush(RGB(45, 45, 48));
    FillRect(hdcMem, &rc, br);
    DeleteObject(br);

    // Extract frame from drag pixels (scaled 4x: 192/48=4, 208/52=4)
    // Simple loop: same as CSS steps(N) infinite
    int n = FRAME_COUNTS[g_anim_state];
    int frame_idx = g_preview_frame % n;
    int fx = frame_idx * FRAME_W;
    int src_row_y = g_anim_state * FRAME_H;
    BYTE *dst = (BYTE*)bits;

    for (int row = 0; row < DRAG_H; row++) {
        int src_row = src_row_y + row * 4;
        for (int col = 0; col < DRAG_W; col++) {
            int src_col = col * 4;
            BYTE *src_pixel = g_drag_pixels + (src_row * g_drag_sheet_w + fx + src_col) * 4;
            BYTE *dst_pixel = dst + (row * DRAG_W + col) * 4;

            BYTE b = src_pixel[0];
            BYTE g = src_pixel[1];
            BYTE r = src_pixel[2];
            BYTE a = src_pixel[3];

            if (a > 64) {
                dst_pixel[0] = b;
                dst_pixel[1] = g;
                dst_pixel[2] = r;
                dst_pixel[3] = a;
            } else {
                dst_pixel[0] = 48;
                dst_pixel[1] = 45;
                dst_pixel[2] = 45;
                dst_pixel[3] = 255;
            }
        }
    }

    SelectObject(hdcMem, hbmp);

    // Update layered window position
    POINT ptSrc = {0, 0};
    POINT ptDst = {x, y};
    SIZE size = {DRAG_W, DRAG_H};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};

    UpdateLayeredWindow(g_drag_window, hdcScreen, &ptDst, &size, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    DeleteObject(hbmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

static void create_desktop_pet(int x, int y, int pet_idx, BYTE *pixels, int sheet_w, int sheet_h) {
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "create_desktop_pet: count=%d, max=%d", g_desktop_pet_count, MAX_DESKTOP_PETS);
        debug_log("%s", buf);
    }
    if (g_desktop_pet_count >= MAX_DESKTOP_PETS) return;

    register_pet_class();

    // Create a new desktop pet window
    HWND hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST,
        L"DesktopPetWindow",
        L"",
        WS_POPUP,
        x, y, DRAG_W, DRAG_H,
        NULL, NULL, g_hinst, NULL);

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "create_desktop_pet: hwnd=%p", (void*)hwnd);
        debug_log("%s", buf);
    }
    if (!hwnd) return;

    // Store pet data
    int id = g_desktop_pet_count;
    g_desktop_pets[id].hwnd = hwnd;
    g_desktop_pets[id].pet_idx = pet_idx;
    g_desktop_pets[id].anim_state = 0;  // idle
    g_desktop_pets[id].frame = 0;
    g_desktop_pets[id].last_frame_time = GetTickCount();
    g_desktop_pets[id].x = x;
    g_desktop_pets[id].y = y;
    g_desktop_pets[id].orig_x = x;
    g_desktop_pets[id].orig_y = y;
    g_desktop_pets[id].dragging = 0;
    g_desktop_pets[id].reaction_end_time = 0;
    g_desktop_pets[id].last_drag_frame_time = GetTickCount();
    g_desktop_pets[id].last_activity_time = GetTickCount();
    g_desktop_pets[id].jump_state = 0;
    g_desktop_pets[id].jump_velocity = 0;
    g_desktop_pets[id].jump_start_y = y;
    g_desktop_pets[id].move_state = 0;
    g_desktop_pets[id].path_type = 0;
    g_desktop_pets[id].path_t = 0;
    g_desktop_pets[id].path_dx = 0;
    g_desktop_pets[id].path_dy = 0;
    g_desktop_pets[id].speed = 0;
    g_desktop_pets[id].center_x = 0;
    g_desktop_pets[id].center_y = 0;
    g_desktop_pets[id].radius_x = 0;
    g_desktop_pets[id].radius_y = 0;
    g_desktop_pets[id].angle = 0;
    g_desktop_pets[id].angle_speed = 0;
    g_desktop_pets[id].start_x = 0;
    g_desktop_pets[id].start_y = 0;
    g_desktop_pets[id].end_x = 0;
    g_desktop_pets[id].end_y = 0;
    g_pet_pixels[id] = pixels;
    g_pet_sheet_w[id] = sheet_w;
    g_pet_sheet_h[id] = sheet_h;
    g_desktop_pet_count++;

    // Install keyboard hook on first pet
    install_keyboard_hook();

    // Set window user data to pet id
    SetWindowLongPtr(hwnd, GWLP_USERDATA, id);

    // Subclass the window to use our proc
    SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)pet_wnd_proc);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);  // Force immediate paint
    SetTimer(hwnd, 1, 50, NULL);

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Desktop pet created at (%d,%d), id=%d, hwnd=%p", x, y, id, (void*)hwnd);
        debug_log("%s", buf);
    }
}

static void end_drag(void) {
    if (!g_dragging) return;

    int was_drag = g_moved;
    int drag_x = g_drag_x;
    int drag_y = g_drag_y;
    int pet_idx = g_drag_pet_idx;
    BYTE *pixels = g_drag_pixels;
    int sheet_w = g_drag_sheet_w;
    int sheet_h = g_drag_sheet_h;

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "end_drag: was_drag=%d, pos=(%d,%d), pet_idx=%d, pixels=%p",
                 was_drag, drag_x, drag_y, pet_idx, (void*)pixels);
        debug_log("%s", buf);
    }

    g_dragging = 0;
    g_drag_pixels = NULL;  // Transfer ownership to desktop pet

    if (g_drag_window) {
        ShowWindow(g_drag_window, SW_HIDE);
        DestroyWindow(g_drag_window);
        g_drag_window = NULL;
    }

    if (was_drag) {
        // Create permanent desktop pet at drag position
        // Center the pet on the cursor position
        create_desktop_pet(drag_x, drag_y, pet_idx, pixels, sheet_w, sheet_h);
    }
    // If it was a click (not drag), just do nothing - the preview handles reaction
}

static void build_sprite_path(wchar_t *out, const wchar_t *base, const char *pet_name, int is_png) {
    wchar_t wname[128];
    MultiByteToWideChar(CP_ACP, 0, pet_name, -1, wname, 128);
    if (is_png)
        wsprintfW(out, L"%ls/%ls/spritesheet.png", base, wname);
    else
        wsprintfW(out, L"%ls/%ls/spritesheet.webp", base, wname);
}

static void draw_preview_frame(void) {
    if (!g_sel || !g_sel->pets || g_sel->pets[g_sel->selected_index].name[0] == '\0') return;

    wchar_t spath[MAX_PATH];
    const wchar_t *base = get_petdex_path_w();
    build_sprite_path(spath, base, g_sel->pets[g_sel->selected_index].name, 0);
    if (GetFileAttributesW(spath) == INVALID_FILE_ATTRIBUTES) {
        build_sprite_path(spath, base, g_sel->pets[g_sel->selected_index].name, 1);
    }

    int current_pet = g_sel->selected_index;
    int current_anim = g_anim_state;
    int frame_idx = 0;

    #ifdef USE_TEST_GIF
    frame_idx = g_preview_frame % 8;
    #else
    // Simple loop: 0→1→2→...→N-1→0→1→... (same as CSS steps(N) infinite)
    {
        int n = FRAME_COUNTS[current_anim];
        frame_idx = g_preview_frame % n;
    }
    #endif

    // Check if we need to reload spritesheet
    int need_reload = (g_cached_pet_idx != current_pet || g_cached_anim_state != current_anim ||
                       wcscmp(g_cached_path, spath) != 0);

    if (need_reload && g_cached_pixels) {
        free(g_cached_pixels);
        g_cached_pixels = NULL;
        g_cached_w = 0;
        g_cached_h = 0;
    }

    if (!g_cached_pixels) {
        // Load spritesheet
        IWICBitmapDecoder *decoder = NULL;
        IWICBitmapFrameDecode *frame = NULL;
        IWICFormatConverter *converter = NULL;
        UINT sheet_w = 0, sheet_h = 0;

        if (!g_wic_factory) {
            CoCreateInstance(&CLSID_WICImagingFactory, NULL,
                CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void**)&g_wic_factory);
        }
        if (g_wic_factory) {
            HRESULT hr = g_wic_factory->lpVtbl->CreateDecoderFromFilename(
                g_wic_factory, spath, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder);
            if (SUCCEEDED(hr)) {
                hr = decoder->lpVtbl->GetFrame(decoder, 0, &frame);
                if (SUCCEEDED(hr)) {
                    hr = frame->lpVtbl->GetSize(frame, &sheet_w, &sheet_h);
                    if (SUCCEEDED(hr)) {
                        hr = g_wic_factory->lpVtbl->CreateFormatConverter(g_wic_factory, &converter);
                        if (SUCCEEDED(hr)) {
                            hr = converter->lpVtbl->Initialize(converter, (IWICBitmapSource*)frame,
                                &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0,
                                WICBitmapPaletteTypeCustom);
                            if (SUCCEEDED(hr)) {
                                g_cached_pixels = calloc(sheet_w * sheet_h, 4);
                                if (g_cached_pixels) {
                                    converter->lpVtbl->CopyPixels(converter, NULL, sheet_w * 4, sheet_w * sheet_h * 4, g_cached_pixels);
                                    g_cached_w = (int)sheet_w;
                                    g_cached_h = (int)sheet_h;
                                    g_cached_pet_idx = current_pet;
                                    g_cached_anim_state = current_anim;
                                    wcscpy(g_cached_path, spath);
                                }
                            }
                        }
                    }
                }
            }
            if (converter) converter->lpVtbl->Release(converter);
            if (frame) frame->lpVtbl->Release(frame);
            if (decoder) decoder->lpVtbl->Release(decoder);
        }
    }

    if (!g_cached_pixels) return;

    // For GIF: decode the specific frame if needed (GIF mode only)
    BYTE *sheet_pixels = g_cached_pixels;
    int sheet_w = g_cached_w;
    int sheet_h = g_cached_h;

    HWND hwnd_preview = g_sel->hwnd_preview;
    HWND hwnd_parent = g_sel->hwnd;
    if (!hwnd_preview || !hwnd_parent) goto cleanup;

    // Get parent DC
    HDC hdc = GetDC(hwnd_parent);
    if (!hdc) goto cleanup;

    // Create memory DC for spritesheet
    HDC hdcSheet = CreateCompatibleDC(hdc);

    // Create DIB for spritesheet (sheet_w x sheet_h)
    BITMAPINFO bmiSheet = {0};
    bmiSheet.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmiSheet.bmiHeader.biWidth = sheet_w;
    bmiSheet.bmiHeader.biHeight = -(int)sheet_h;  // top-down
    bmiSheet.bmiHeader.biPlanes = 1;
    bmiSheet.bmiHeader.biBitCount = 32;
    bmiSheet.bmiHeader.biCompression = BI_RGB;

    void *sheet_bits = NULL;
    HBITMAP hbmpSheet = CreateDIBSection(hdcSheet, &bmiSheet, DIB_RGB_COLORS, &sheet_bits, NULL, 0);
    if (!hbmpSheet || !sheet_bits) {
        DeleteDC(hdcSheet);
        ReleaseDC(hwnd_parent, hdc);
        goto cleanup;
    }

    // Copy spritesheet pixels
    memcpy(sheet_bits, sheet_pixels, sheet_w * sheet_h * 4);
    HGDIOBJ oldSheet = SelectObject(hdcSheet, hbmpSheet);

    // Create memory DC for frame output
    HDC hdcMem = CreateCompatibleDC(hdc);

    // Create DIB for 192x208 frame
    BITMAPINFO bmiFrame = {0};
    bmiFrame.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmiFrame.bmiHeader.biWidth = FRAME_W;
    bmiFrame.bmiHeader.biHeight = -FRAME_H;
    bmiFrame.bmiHeader.biPlanes = 1;
    bmiFrame.bmiHeader.biBitCount = 32;
    bmiFrame.bmiHeader.biCompression = BI_RGB;

    void *frame_bits = NULL;
    HBITMAP hbmpFrame = CreateDIBSection(hdcMem, &bmiFrame, DIB_RGB_COLORS, &frame_bits, NULL, 0);
    if (!hbmpFrame || !frame_bits) {
        SelectObject(hdcSheet, oldSheet);
        DeleteObject(hbmpSheet);
        DeleteDC(hdcSheet);
        DeleteDC(hdcMem);
        ReleaseDC(hwnd_parent, hdc);
        goto cleanup;
    }

    HGDIOBJ oldFrame = SelectObject(hdcMem, hbmpFrame);

    // Fill frame with gray background
    RECT rc = {0, 0, FRAME_W, FRAME_H};
    HBRUSH br = CreateSolidBrush(RGB(45, 45, 48));
    FillRect(hdcMem, &rc, br);
    DeleteObject(br);

    #ifdef USE_TEST_GIF
    // GIF: copy full frame (already 192x208)
    memcpy(frame_bits, sheet_bits, sheet_w * sheet_h * 4);
    #else
    // Spritesheet: extract frame from sheet
    {
    int fx = frame_idx * FRAME_W;
    int src_row_y = g_anim_state * FRAME_H;

    int src_stride = sheet_w * 4;
    int dst_stride = FRAME_W * 4;

    BYTE *src = (BYTE*)sheet_bits + (src_row_y * (int)sheet_w + fx) * 4;
    BYTE *dst = (BYTE*)frame_bits;

    for (int row = 0; row < FRAME_H; row++) {
        // Copy one row at a time
        BYTE *src_row = src + row * src_stride;
        BYTE *dst_row = dst + row * dst_stride;
        for (int col = 0; col < FRAME_W; col++) {
            // BGRA format
            BYTE b = src_row[col * 4];
            BYTE g = src_row[col * 4 + 1];
            BYTE r = src_row[col * 4 + 2];
            BYTE a = src_row[col * 4 + 3];

            if (a > 64) {
                // Semi-opaque or opaque - copy pixel
                dst_row[col * 4] = b;
                dst_row[col * 4 + 1] = g;
                dst_row[col * 4 + 2] = r;
                dst_row[col * 4 + 3] = a;
            } else {
                // Transparent - leave as background (gray: B=48, G=45, R=45)
                dst_row[col * 4] = 48;     // B
                dst_row[col * 4 + 1] = 45; // G
                dst_row[col * 4 + 2] = 45; // R
                dst_row[col * 4 + 3] = 255; // A (opaque for the background)
            }
        }
    }
    }
    #endif

    // Copy to screen at preview position (230, 60)
    BitBlt(hdc, 230, 60, FRAME_W, FRAME_H, hdcMem, 0, 0, SRCCOPY);

    // Cleanup
    SelectObject(hdcMem, oldFrame);
    SelectObject(hdcSheet, oldSheet);
    DeleteObject(hbmpFrame);
    DeleteObject(hbmpSheet);
    DeleteDC(hdcMem);
    DeleteDC(hdcSheet);
    ReleaseDC(hwnd_parent, hdc);

cleanup:
    // Note: sheet_pixels points to g_cached_pixels - don't free here
    // converter/frame/decoder are released immediately after loading (see above)
    (void)0;  // placeholder
}

static LRESULT CALLBACK selector_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND: {
            HDC hdc = (HDC)wp;
            RECT rc;
            GetClientRect(hwnd, &rc);
            HBRUSH br = CreateSolidBrush(RGB(255, 255, 255));
            FillRect(hdc, &rc, br);
            DeleteObject(br);
            return 1;
        }

        case WM_CREATE: {
            CREATESTRUCT *cs = (CREATESTRUCT*)lp;
            g_sel = (PetSelector*)cs->lpCreateParams;
            g_sel->hwnd = hwnd;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)g_sel);
            g_preview_frame = 0;

            HDC hdc = GetDC(hwnd);
            int dpi = GetDeviceCaps(hdc, LOGPIXELSX);
            ReleaseDC(hwnd, hdc);
            int fs = MulDiv(9, dpi, 72);

            HFONT hFont = CreateFontW(fs, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

            g_sel->hwnd_list = CreateWindowW(L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL | WS_BORDER,
                15, 15, 200, 260, hwnd, (HMENU)1, g_hinst, NULL);
            SendMessageW(g_sel->hwnd_list, WM_SETFONT, (WPARAM)hFont, TRUE);

            // Create preview static (transparent background via WM_CTLCOLORSTATIC)
            g_sel->hwnd_preview = CreateWindowW(L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_NOTIFY,
                230, 60, 192, 208, hwnd, (HMENU)2, g_hinst, NULL);

            HFONT btn_font = CreateFontW(fs + 2, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            g_sel->hwnd_start = CreateWindowW(L"BUTTON", L"Start!",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_CENTER,
                230, 15, 192, 30, hwnd, (HMENU)3, g_hinst, NULL);
            SendMessageW(g_sel->hwnd_start, WM_SETFONT, (WPARAM)btn_font, TRUE);

            for (int i = 0; i < g_sel->pet_count; i++) {
                const char *display = g_sel->pets[i].display_name[0]
                    ? g_sel->pets[i].display_name : g_sel->pets[i].name;
                wchar_t *wide = utf8_to_wide(display);
                if (wide) {
                    SendMessageW(g_sel->hwnd_list, LB_ADDSTRING, 0, (LPARAM)wide);
                    free(wide);
                }
            }
            SendMessageW(g_sel->hwnd_list, LB_SETCURSEL, 0, 0);
            g_last_frame_time = GetTickCount();
            g_last_state_time = GetTickCount();
            g_idle_cycle_idx = 0;
            g_anim_state = IDLE_CYCLE[0];
            SetTimer(hwnd, 1, 50, NULL);
            PostMessageW(hwnd, WM_TIMER, 1, 0);
            return 0;
        }

        case WM_TIMER:
            if (wp == 1) {
                DWORD now = GetTickCount();

                // Handle based on current state
                switch (g_preview_state) {
                    case STATE_IDLE_CYCLE: {
                        // Random idle cycle timing
                        int wait = IDLE_TICK_MIN_MS + (rand() % (IDLE_TICK_MAX_MS - IDLE_TICK_MIN_MS));
                        if (now - g_last_state_time >= (DWORD)wait) {
                            g_idle_cycle_idx = (g_idle_cycle_idx + 1) % IDLE_CYCLE_COUNT;
                            g_anim_state = IDLE_CYCLE[g_idle_cycle_idx];
                            g_preview_frame = 0;
                            g_last_state_time = now;
                            draw_preview_frame();
                        }
                        break;
                    }
                    case STATE_REACTION: {
                        // Reaction lasts ~1100ms then back to idle
                        if (now - g_last_state_time >= REACTION_MS) {
                            g_preview_state = STATE_IDLE_CYCLE;
                            g_last_state_time = now;
                        }
                        break;
                    }
                    case STATE_DRAGGING:
                        // State is set directly during drag, no timing needed
                        break;
                    case STATE_COOLDOWN: {
                        // Cooldown ~600ms then back to idle cycle
                        if (now - g_last_state_time >= RUN_TAIL_MS) {
                            g_preview_state = STATE_IDLE_CYCLE;
                            g_idle_cycle_idx = 0;
                            g_anim_state = IDLE_CYCLE[0];
                            g_last_state_time = now;
                            draw_preview_frame();
                        }
                        break;
                    }
                }

                // Advance frame animation regardless of state
                int frame_duration = FRAME_DURATIONS[g_anim_state];
                if (now - g_last_frame_time >= (DWORD)frame_duration) {
                    g_preview_frame++;
                    g_last_frame_time = now;
                    draw_preview_frame();
                }
            }
            return 0;

        case WM_LBUTTONDOWN: {
            int x = LOWORD(lp);
            int y = HIWORD(lp);
            // Check if click is in preview area (230, 60, 192, 208)
            if (x >= 230 && x < 230 + 192 && y >= 60 && y < 60 + 208) {
                // Store initial position for click vs drag detection
                g_click_start_x = x;
                g_click_start_y = y;
                g_moved = 0;
                g_last_drag_x = x;

                // Start drag - center the 96x104 drag window on click position
                RECT rc;
                GetWindowRect(hwnd, &rc);
                start_drag(rc.left + x - DRAG_W/2, rc.top + y - DRAG_H/2);
                SetCapture(hwnd);
                g_preview_state = STATE_DRAGGING;
                g_last_state_time = GetTickCount();
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (g_dragging) {
                int x = LOWORD(lp);
                int y = HIWORD(lp);

                // Check if significant movement occurred (threshold: 4px per Codex)
                int dx = x - g_click_start_x;
                int dy = y - g_click_start_y;
                if (abs(dx) + abs(dy) > 4) {
                    g_moved = 1;
                }

                RECT rc;
                GetWindowRect(hwnd, &rc);
                // Center the drag window on mouse position
                int screen_x = rc.left + x - DRAG_W/2;
                int screen_y = rc.top + y - DRAG_H/2;
                update_drag(screen_x, screen_y);

                // Direction-aware running state
                int drag_dx = x - g_last_drag_x;
                if (drag_dx > 1) {
                    g_anim_state = 1;  // running-right (row 1)
                    g_preview_frame = 0;
                } else if (drag_dx < -1) {
                    g_anim_state = 2;  // running-left (row 2)
                    g_preview_frame = 0;
                }
                g_last_drag_x = x;
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            debug_log("WM_LBUTTONUP reached!");
            {
                char buf[256];
                snprintf(buf, sizeof(buf), "WM_LBUTTONUP: g_dragging=%d, g_moved=%d", g_dragging, g_moved);
                debug_log("%s", buf);
            }
            if (g_dragging) {
                {
                    char buf[256];
                    snprintf(buf, sizeof(buf), "WM_LBUTTONUP: g_dragging=1, g_moved=%d, g_drag_x=%d, g_drag_y=%d",
                             g_moved, g_drag_x, g_drag_y);
                    debug_log("%s", buf);
                }
                end_drag();
                ReleaseCapture();

                if (!g_moved) {
                    // It was a click - trigger reaction (waving/jumping alternating)
                    g_preview_state = STATE_REACTION;
                    g_reaction_toggle = !g_reaction_toggle;
                    g_anim_state = g_reaction_toggle ? 3 : 4;  // waving or jumping
                    g_preview_frame = 0;
                    g_last_state_time = GetTickCount();
                } else {
                    // It was a drag - cooldown then back to idle
                    g_preview_state = STATE_COOLDOWN;
                    g_last_state_time = GetTickCount();
                    g_anim_state = IDLE_CYCLE[0];  // idle
                    g_preview_frame = 0;
                }
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = (HDC)wp;
            SetBkMode(hdcStatic, TRANSPARENT);
            return (LRESULT)GetStockObject(HOLLOW_BRUSH);
        }

        case WM_KEYDOWN: {
            int key = (int)wp;
            if (key == VK_ESCAPE) {
                DestroyWindow(hwnd);
            } else if (key == VK_UP) {
                // Previous pet in list
                int new_idx = g_sel->selected_index - 1;
                if (new_idx < 0) new_idx = g_sel->pet_count - 1;
                g_sel->selected_index = new_idx;
                g_preview_frame = 0;
                SendMessageW(g_sel->hwnd_list, LB_SETCURSEL, new_idx, 0);
            } else if (key == VK_DOWN) {
                // Next pet in list
                int new_idx = g_sel->selected_index + 1;
                if (new_idx >= g_sel->pet_count) new_idx = 0;
                g_sel->selected_index = new_idx;
                g_preview_frame = 0;
                SendMessageW(g_sel->hwnd_list, LB_SETCURSEL, new_idx, 0);
            } else if (key == VK_LEFT) {
                // Previous animation state (row)
                g_anim_state = (g_anim_state - 1 + (int)(sizeof(FRAME_COUNTS)/sizeof(FRAME_COUNTS[0]))) % (int)(sizeof(FRAME_COUNTS)/sizeof(FRAME_COUNTS[0]));
                g_preview_frame = 0;
            } else if (key == VK_RIGHT) {
                // Next animation state (row)
                g_anim_state = (g_anim_state + 1) % (int)(sizeof(FRAME_COUNTS)/sizeof(FRAME_COUNTS[0]));
                g_preview_frame = 0;
            }
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wp);
            int ev = HIWORD(wp);
            if (id == 1 && ev == LBN_SELCHANGE) {
                int idx = SendMessageW(g_sel->hwnd_list, LB_GETCURSEL, 0, 0);
                if (idx != LB_ERR) {
                    g_sel->selected_index = idx;
                    g_preview_frame = 0;
                }
            }
            if (id == 1 && ev == LBN_DBLCLK) {
                g_exit_code = g_sel->selected_index + 1;
                DestroyWindow(hwnd);
            }
            if (id == 3 && ev == BN_CLICKED) {
                g_exit_code = g_sel->selected_index + 1;
                DestroyWindow(hwnd);
            }
            // Preview click (STN_CLICKED = 0 from STATIC id=2)
            if (id == 2 && ev == 0) {
                // Get cursor position for drag start
                POINT pt;
                GetCursorPos(&pt);
                RECT rc;
                GetWindowRect(hwnd, &rc);
                int x = pt.x - rc.left;
                int y = pt.y - rc.top;
                g_click_start_x = x;
                g_click_start_y = y;
                g_moved = 0;
                g_last_drag_x = x;
                start_drag(rc.left + x - DRAG_W/2, rc.top + y - DRAG_H/2);
                SetCapture(hwnd);
                g_preview_state = STATE_DRAGGING;
                g_last_state_time = GetTickCount();
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

PetSelector *create_pet_selector(void) {
    PetInfo *pets = NULL;
    int count = get_all_pets(&pets);
    PetSelector *sel = calloc(1, sizeof(PetSelector));
    sel->pets = pets;
    sel->pet_count = count;
    sel->selected_index = 0;
    return sel;
}

void destroy_pet_selector(PetSelector *sel) {
    if (!sel) return;
    free_pet_list(sel->pets);
    free(sel);
}

int run_pet_selector(PetSelector *sel) {
    if (!sel || sel->pet_count == 0) return -1;
    g_sel = sel;
    g_exit_code = -1;

    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);
    int win_w = 437;
    int win_h = 290;

    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = selector_wnd_proc;
    wc.hInstance = g_hinst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"PetSelectorClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"PetSelectorClass", L"Selector",
        WS_POPUP,
        (screen_w - win_w)/2, (screen_h - win_h)/2,
        win_w, win_h, NULL, NULL, g_hinst, sel);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return g_exit_code;
}
