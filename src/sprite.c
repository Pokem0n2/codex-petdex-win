#include "sprite.h"
#include <wincodec.h>
#include <stdio.h>
#define __USE_MINGW_ANSI_STDIO 1

IWICImagingFactory *g_wic_factory = NULL;

static HRESULT init_factory(void) {
    if (g_wic_factory) return S_OK;
    return CoCreateInstance(
        &CLSID_WICImagingFactory, NULL,
        CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory,
        (void**)&g_wic_factory
    );
}

static const int FRAME_W = 192;
static const int FRAME_H = 208;
static const int FRAME_COUNTS[STATE_COUNT] = {6, 8, 8, 4, 5, 8, 6, 6, 6};

static HRESULT detect_frame_boundaries(Pet *pet) {
    for (int row = 0; row < STATE_COUNT; row++) {
        int count = FRAME_COUNTS[row];
        pet->states[row].frames = calloc(count, sizeof(Frame));
        pet->states[row].count = count;
        pet->states[row].current = 0;

        for (int col = 0; col < count; col++) {
            pet->states[row].frames[col].x = col * FRAME_W;
            pet->states[row].frames[col].width = FRAME_W;
            pet->states[row].frames[col].height = FRAME_H;
        }
    }
    return S_OK;
}

BOOL load_spritesheet(Pet *pet, const wchar_t *path) {
    HRESULT hr;

    hr = init_factory();
    if (FAILED(hr)) return FALSE;

    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *converter = NULL;

    hr = g_wic_factory->lpVtbl->CreateDecoderFromFilename(
        g_wic_factory, path, NULL, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, &decoder);
    if (FAILED(hr)) goto cleanup;

    hr = decoder->lpVtbl->GetFrame(decoder, 0, &frame);
    if (FAILED(hr)) goto cleanup;

    UINT w, h;
    hr = frame->lpVtbl->GetSize(frame, &w, &h);
    if (FAILED(hr)) goto cleanup;

    hr = g_wic_factory->lpVtbl->CreateFormatConverter(g_wic_factory, &converter);
    if (FAILED(hr)) goto cleanup;

    hr = converter->lpVtbl->Initialize(
        converter, (IWICBitmapSource*)frame,
        &GUID_WICPixelFormat32bppBGRA,
        WICBitmapDitherTypeNone, NULL, 0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) goto cleanup;

    pet->sheet_w = (int)w;
    pet->sheet_h = (int)h;
    pet->pixels = malloc(w * h * 4);
    if (!pet->pixels) { hr = E_OUTOFMEMORY; goto cleanup; }

    hr = converter->lpVtbl->CopyPixels(
        converter, NULL, w * 4, w * h * 4, pet->pixels);
    if (FAILED(hr)) goto cleanup;

    // Clear RGB channels where alpha=0 (WIC may leave garbage in transparent regions)
    for (int i = 0; i < (int)(w * h * 4); i += 4) {
        if (pet->pixels[i + 3] == 0) {
            pet->pixels[i] = 0;
            pet->pixels[i + 1] = 0;
            pet->pixels[i + 2] = 0;
        }
    }

    detect_frame_boundaries(pet);
    pet->current_state = STATE_IDLE;

cleanup:
    if (converter) converter->lpVtbl->Release(converter);
    if (frame) frame->lpVtbl->Release(frame);
    if (decoder) decoder->lpVtbl->Release(decoder);

    return SUCCEEDED(hr);
}

void free_spritesheet(Pet *pet) {
    if (pet->pixels) {
        free(pet->pixels);
        pet->pixels = NULL;
    }
    for (int i = 0; i < STATE_COUNT; i++) {
        if (pet->states[i].frames) {
            free(pet->states[i].frames);
            pet->states[i].frames = NULL;
        }
    }
}
