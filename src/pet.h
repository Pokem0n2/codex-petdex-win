#ifndef PET_H
#define PET_H

#include <windows.h>

#define STATE_COUNT 9

enum PetState {
    STATE_IDLE = 0,
    STATE_RUN_RIGHT,
    STATE_RUN_LEFT,
    STATE_WAVING,
    STATE_JUMPING,
    STATE_FAILED,
    STATE_WAITING,
    STATE_RUNNING,
    STATE_REVIEW
};

typedef struct {
    char name[64];
    int x;
    int width;
    int height;
} Frame;

typedef struct {
    Frame *frames;
    int count;
    int current;
} AnimState;

typedef struct {
    char name[64];
    char display_name[64];
    int sheet_w;
    int sheet_h;
    BYTE *pixels;
    AnimState states[STATE_COUNT];
    int current_state;
} Pet;

typedef struct {
    char name[64];
    char display_name[64];
} PetInfo;

int get_all_pets(PetInfo **out_pets);
void free_pet_list(PetInfo *pets);

const char *state_name(int state);
const wchar_t *get_petdex_path_w(void);

#endif
