// Animated weather icon for the dashboard: Twemoji sun/moon/cloud images plus small LVGL objects
// (drops, flakes, fog bars, lightning) moved by looping animations.
#include "app.h"
#include "lvgl.h"
#include "emoji/emoji.h"

#include <stdint.h>

#define BOX_W   46
#define BOX_H   44
#define SMALL   1   // emoji size index for the small sun/moon/lightning (22 px)

// the box remembers what it shows, so a weather update with the same icon doesn't restart it
#define SHOWN(kind, day) ((void *)(intptr_t)(1 + (kind) * 2 + (day)))

static lv_obj_t *image(lv_obj_t *box, const lv_image_dsc_t *src, int x, int y)
{
    lv_obj_t *img = lv_image_create(box);
    lv_image_set_src(img, src);
    lv_obj_set_pos(img, x, y);
    return img;
}

static lv_obj_t *blob(lv_obj_t *box, int w, int h, uint32_t color, int x, int y)
{
    lv_obj_t *o = lv_obj_create(box);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_pos(o, x, y);
    return o;
}

static void loop(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int from, int to, int ms, int delay, bool back)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    if (back) {
        lv_anim_set_reverse_duration(&a, ms);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    }
    lv_anim_start(&a);
}

static void set_x(void *o, int32_t v)
{
    lv_obj_set_x(o, v);
}

static void set_y(void *o, int32_t v)
{
    lv_obj_set_y(o, v);
}

static void set_bg_opa(void *o, int32_t v)
{
    lv_obj_set_style_bg_opa(o, v, 0);
}

// falling drop/flake: v 0..100 along the fall, fading out over the last third
static void fall(void *o, int32_t v)
{
    intptr_t d = (intptr_t)lv_obj_get_user_data(o);
    int y0 = d & 0xff, len = d >> 8;
    lv_obj_set_y(o, y0 + v * len / 100);
    lv_obj_set_style_opa(o, v < 66 ? LV_OPA_COVER : (100 - v) * 255 / 34, 0);
}

// lightning: two quick flashes per cycle
static void flash(void *o, int32_t v)
{
    bool on = (v >= 60 && v < 64) || (v >= 70 && v < 76);
    lv_obj_set_style_image_opa(o, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void drops(lv_obj_t *box, const int *xs, int n, int y0, int len, int w, int h, uint32_t color, int ms)
{
    for (int i = 0; i < n; i++) {
        lv_obj_t *d = blob(box, w, h, color, xs[i], y0);
        lv_obj_set_user_data(d, (void *)(intptr_t)(y0 | (len << 8)));
        lv_obj_set_style_opa(d, LV_OPA_TRANSP, 0);
        loop(d, fall, 0, 100, ms, i * ms / n, false);
    }
}

static lv_obj_t *cloud(lv_obj_t *box, int x, int y, int drift)
{
    lv_obj_t *c = image(box, &EMOJI_BIG[EMOJI_BIG_CLOUD], x, y);
    if (drift) {
        loop(c, set_x, x, x + drift, 3000, 0, true);
    }
    return c;
}

lv_obj_t *weather_icon_create(lv_obj_t *parent)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, BOX_W, BOX_H);
    return box;
}

void weather_icon_set(lv_obj_t *box, int kind, bool day)
{
    if (lv_obj_get_user_data(box) == SHOWN(kind, day)) {
        return;
    }
    lv_obj_set_user_data(box, SHOWN(kind, day));
    lv_obj_clean(box);   // also stops the old animations

    const lv_image_dsc_t *small = emoji_image(day ? 0x2600 : 0x1F319, SMALL);
    switch (kind) {
    case WX_CLEAR:
        if (day) {
            lv_obj_t *glow = blob(box, 42, 42, 0xffd54f, 2, 1);
            loop(glow, set_bg_opa, 20, 90, 1600, 0, true);
            image(box, &EMOJI_BIG[EMOJI_BIG_SUN], 5, 4);
        } else {
            lv_obj_t *moon = image(box, &EMOJI_BIG[EMOJI_BIG_MOON], 5, 3);
            loop(moon, set_y, 2, 6, 2500, 0, true);
        }
        break;
    case WX_PARTLY:
        image(box, small, 22, 0);
        cloud(box, 0, 8, 6);
        break;
    case WX_CLOUDY: {
        lv_obj_t *back = image(box, emoji_image(0x2601, SMALL), 0, 2);
        loop(back, set_x, 0, 5, 3700, 0, true);
        cloud(box, 10, 6, -6);
        break;
    }
    case WX_FOG:
        cloud(box, 5, 0, 4);
        for (int i = 0; i < 3; i++) {
            lv_obj_t *bar = blob(box, 30, 3, 0xb0bec5, 8, 31 + i * 5);
            loop(bar, set_x, 4 + (i % 2) * 6, 10 - (i % 2) * 6, 2200, i * 300, true);
        }
        break;
    case WX_DRIZZLE: {
        cloud(box, 5, 0, 4);
        static const int xs[] = {14, 23, 32};
        drops(box, xs, 3, 28, 14, 2, 5, 0x81d4fa, 1500);
        break;
    }
    case WX_RAIN: {
        cloud(box, 5, 0, 4);
        static const int xs[] = {11, 17, 23, 29, 35};
        drops(box, xs, 5, 28, 15, 2, 7, 0x42a5f5, 800);
        break;
    }
    case WX_SHOWERS: {
        image(box, small, 22, 0);
        cloud(box, 0, 6, 4);
        static const int xs[] = {9, 17, 25, 33};
        drops(box, xs, 4, 32, 11, 2, 7, 0x1e88e5, 650);
        break;
    }
    case WX_SNOW: {
        cloud(box, 5, 0, 4);
        static const int xs[] = {12, 20, 28, 36};
        drops(box, xs, 4, 28, 14, 5, 5, 0xffffff, 2400);
        break;
    }
    default: {   // thunderstorm
        cloud(box, 5, 0, 0);
        static const int xs[] = {12, 34};
        drops(box, xs, 2, 28, 14, 2, 7, 0x42a5f5, 800);
        lv_obj_t *bolt = image(box, emoji_image(0x26A1, 0), 17, 26);
        lv_obj_set_style_image_opa(bolt, LV_OPA_TRANSP, 0);
        loop(bolt, flash, 0, 100, 2600, 0, false);
        break;
    }
    }
}
