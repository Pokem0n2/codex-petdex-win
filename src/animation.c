#include "animation.h"

void advance_animation(Pet *pet) {
    AnimState *s = &pet->states[pet->current_state];
    if (s->count == 0) return;
    s->current = (s->current + 1) % s->count;
}

void set_state(Pet *pet, int state) {
    if (state < 0 || state >= STATE_COUNT) return;
    pet->current_state = state;
}
