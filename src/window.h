#ifndef WINDOW_H
#define WINDOW_H

#include "pet.h"

extern HINSTANCE g_hinst;
extern Pet *g_pet;

HWND create_layered_window(Pet *pet, PetInfo *pet_list, int pet_count, int initial_idx);
void render_frame(HWND hwnd, Pet *pet);
void cleanup_window(HWND hwnd, Pet *pet);

#endif
