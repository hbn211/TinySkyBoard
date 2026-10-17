#include "emoji.h"

LV_FONT_DECLARE(font_pt_14);
LV_FONT_DECLARE(font_pt_20);
LV_FONT_DECLARE(font_pt_28);

typedef struct {
    const lv_font_t *base;   // the text font: font_pt_14/20/28
    lv_font_t font;          // copy of it with the emoji as fallback
    int size;                // index into EMOJI[].img
} emoji_font_t;

static emoji_font_t s_fonts[EMOJI_N_SIZES] = {
    {.base = &font_pt_14, .size = 0},
    {.base = &font_pt_20, .size = 1},
    {.base = &font_pt_28, .size = 2},
};
static bool s_ready;

static const emoji_t *find(uint32_t cp)
{
    int lo = 0, hi = EMOJI_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (EMOJI[mid].cp == cp) {
            return &EMOJI[mid];
        }
        if (EMOJI[mid].cp < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

bool emoji_known(uint32_t cp)
{
    return find(cp) != NULL;
}

const lv_image_dsc_t *emoji_image(uint32_t cp, int size)
{
    const emoji_t *e = find(cp);
    return e && size >= 0 && size < EMOJI_N_SIZES ? &e->img[size] : NULL;
}

static const void *image_for(const lv_font_t *font, uint32_t unicode, uint32_t unicode_next, int32_t *offset_y,
                             void *user_data)
{
    const emoji_font_t *ef = user_data;
    const emoji_t *e = find(unicode);
    if (!e) {
        return NULL;
    }
    // the glyph is placed with the text font's metrics: center the image on its line
    int s = EMOJI_SIZES[ef->size];
    int lh = ef->base->line_height;
    *offset_y = (lh - ef->base->base_line) - s - (lh - s) / 2;
    return &e->img[ef->size];
}

const lv_font_t *emoji_font(const lv_font_t *font)
{
    if (!s_ready) {
        for (int i = 0; i < EMOJI_N_SIZES; i++) {
            s_fonts[i].font = *s_fonts[i].base;
            s_fonts[i].font.fallback = lv_imgfont_create(s_fonts[i].base->line_height, image_for, &s_fonts[i]);
        }
        s_ready = true;
    }
    for (int i = 0; i < EMOJI_N_SIZES; i++) {
        if (font == s_fonts[i].base || font == &s_fonts[i].font) {
            return &s_fonts[i].font;
        }
    }
    return font;
}
