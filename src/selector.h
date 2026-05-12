#ifndef SELECTOR_H
#define SELECTOR_H

#include <windows.h>
#include "pet.h"

typedef struct {
    HWND hwnd;
    HWND hwnd_list;
    HWND hwnd_preview;
    HWND hwnd_start;
    PetInfo *pets;
    int pet_count;
    int selected_index;
} PetSelector;

PetSelector *create_pet_selector(void);
int run_pet_selector(PetSelector *sel);
void destroy_pet_selector(PetSelector *sel);

#endif
