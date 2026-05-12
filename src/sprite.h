#ifndef SPRITE_H
#define SPRITE_H

#include <windows.h>
#include <wincodec.h>
#include "pet.h"

extern IWICImagingFactory *g_wic_factory;

BOOL load_spritesheet(Pet *pet, const wchar_t *path);
void free_spritesheet(Pet *pet);

#endif
