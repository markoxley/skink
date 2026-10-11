/* ui_rt.c — Skink UI runtime shim.
 *
 * Bridges `std/ui` to a vendored Nuklear immediate-mode GUI rendered by
 * SDL2's SDL_Renderer. All state is window-scoped and addressed by the
 * opaque int64 handles returned here; Skink code never sees C pointers.
 *
 * This file is compiled and linked ONLY when the compiled program imports
 * `std/ui` (see skink.skink's conditional-link logic) so non-GUI binaries
 * carry no SDL2 dependency.
 */

#define NK_IMPLEMENTATION
#define NK_SDL_RENDERER_IMPLEMENTATION
#define NK_SDL_RENDERER_SDL_H <SDL2/SDL.h>
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT

#include <SDL2/SDL.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "vendor/nuklear.h"
#include "vendor/nuklear_sdl_renderer.h"

/* ------------------------------------------------------------------ */
/* Window registry                                                     */
/* ------------------------------------------------------------------ */

#define UI_MAX_WINS   16
#define UI_MAX_EDITS  256
#define UI_MAX_TREES  128
#define UI_EDIT_CAP   2048

typedef struct {
    int   id;
    char  text[UI_EDIT_CAP];
    int   len;
} UiEdit;

typedef struct {
    int   id;
    enum nk_collapse_states open; /* MINIMIZED/MAXIMIZED */
} UiTree;

typedef struct {
    SDL_Window          *win;
    SDL_Renderer        *ren;
    struct nk_context   *ctx;
    int                  quit;
    /* Text-edit widget state, keyed by the widget id Skink hands down. */
    UiEdit               edits[UI_MAX_EDITS];
    int                  nEdits;
    /* Persisted tree-node open/closed state, keyed by widget id. */
    UiTree               trees[UI_MAX_TREES];
    int                  nTrees;
    /* nk_list_view holds state between begin/end — keep it per window. */
    struct nk_list_view  lv;
    int                  lvActive;
} UiWin;

static UiWin *gWins[UI_MAX_WINS];
static int    gSdlUp = 0;

static UiWin *winOf(long long h) { return (UiWin *)(intptr_t)h; }
static struct nk_context *ctxOf(long long h) { return winOf(h)->ctx; }

static UiEdit *editFor(UiWin *w, int id) {
    int i;
    for (i = 0; i < w->nEdits; i++)
        if (w->edits[i].id == id) return &w->edits[i];
    if (w->nEdits >= UI_MAX_EDITS) return &w->edits[0]; /* saturate: reuse 0 */
    w->edits[w->nEdits].id  = id;
    w->edits[w->nEdits].len = 0;
    w->edits[w->nEdits].text[0] = '\0';
    return &w->edits[w->nEdits++];
}

static enum nk_collapse_states *treeStateFor(UiWin *w, int id) {
    int i;
    for (i = 0; i < w->nTrees; i++)
        if (w->trees[i].id == id) return &w->trees[i].open;
    if (w->nTrees >= UI_MAX_TREES) return &w->trees[0].open;
    w->trees[w->nTrees].id   = id;
    w->trees[w->nTrees].open = NK_MINIMIZED;
    return &w->trees[w->nTrees++].open;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/* Skink_ui_init brings up SDL's video subsystem once. Returns 1 on ok. */
long long Skink_ui_init(void) {
    if (gSdlUp) return 1;
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0)
        return 0;
    gSdlUp = 1;
    return 1;
}

/* Skink_ui_window_create opens a window + GPU renderer + nk context.
 * flags bit 0 = resizable, bit 1 = borderless, bit 2 = fullscreen-desktop. */
long long Skink_ui_window_create(const char *title, int w, int h, int flags) {
    int i; UiWin *uw; Uint32 sdlFlags = SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI;
    if (!gSdlUp && !Skink_ui_init()) return 0;
    for (i = 0; i < UI_MAX_WINS; i++) if (!gWins[i]) break;
    if (i == UI_MAX_WINS) return 0;
    if (flags & 1) sdlFlags |= SDL_WINDOW_RESIZABLE;
    if (flags & 2) sdlFlags |= SDL_WINDOW_BORDERLESS;
    if (flags & 4) sdlFlags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    uw = (UiWin *)calloc(1, sizeof(UiWin));
    uw->win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, w, h, sdlFlags);
    if (!uw->win) { free(uw); return 0; }
    uw->ren = SDL_CreateRenderer(uw->win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!uw->ren) /* fall back to software so headless CI still works */
        uw->ren = SDL_CreateRenderer(uw->win, -1, SDL_RENDERER_SOFTWARE);
    if (!uw->ren) { SDL_DestroyWindow(uw->win); free(uw); return 0; }

    uw->ctx = nk_sdl_init(uw->win, uw->ren);
    {
        struct nk_font_atlas *atlas;
        struct nk_font *font;
        nk_sdl_font_stash_begin(&atlas);
        font = nk_font_atlas_add_default(atlas, 14, 0);
        nk_sdl_font_stash_end();
        nk_style_set_font(uw->ctx, &font->handle);
    }
    gWins[i] = uw;
    return (long long)(intptr_t)uw;
}

/* Skink_ui_window_destroy tears down one window. */
void Skink_ui_window_destroy(long long h) {
    int i; UiWin *w = winOf(h);
    for (i = 0; i < UI_MAX_WINS; i++) if (gWins[i] == w) gWins[i] = 0;
    if (!w) return;
    nk_sdl_shutdown();  /* shared renderer glue; last window owns it */
    nk_free(w->ctx);
    SDL_DestroyRenderer(w->ren);
    SDL_DestroyWindow(w->win);
    free(w);
}

/* Skink_ui_frame_begin pumps events, opens the root panel, and returns
 * 1 while the window should keep running, 0 after a close request. */
long long Skink_ui_frame_begin(long long h) {
    UiWin *w = winOf(h);
    SDL_Event evt;
    int ww, wh;
    nk_input_begin(w->ctx);
    while (SDL_PollEvent(&evt)) {
        if (evt.type == SDL_QUIT) w->quit = 1;
        if (evt.type == SDL_WINDOWEVENT &&
            evt.window.event == SDL_WINDOWEVENT_CLOSE &&
            evt.window.windowID == SDL_GetWindowID(w->win)) w->quit = 1;
        nk_sdl_handle_event(&evt);
    }
    nk_input_end(w->ctx);
    if (w->quit) return 0;
    SDL_GetWindowSize(w->win, &ww, &wh);
    if (nk_begin(w->ctx, "main", nk_rect(0, 0, (float)ww, (float)wh), 0) == 0)
        return 0;
    return 1;
}

void Skink_ui_frame_end(long long h) {
    UiWin *w = winOf(h);
    nk_end(w->ctx);
    SDL_SetRenderDrawColor(w->ren, 25, 25, 30, 255);
    SDL_RenderClear(w->ren);
    nk_sdl_render(NK_ANTI_ALIASING_ON);
    SDL_RenderPresent(w->ren);
}

long long Skink_ui_should_close(long long h) { return winOf(h)->quit; }
void      Skink_ui_request_close(long long h) { winOf(h)->quit = 1; }
void      Skink_ui_set_title(long long h, const char *t) { SDL_SetWindowTitle(winOf(h)->win, t); }
long long Skink_ui_window_w(long long h) { int a, b; SDL_GetWindowSize(winOf(h)->win, &a, &b); return a; }
long long Skink_ui_window_h(long long h) { int a, b; SDL_GetWindowSize(winOf(h)->win, &a, &b); return b; }

/* Clipboard helpers for the binding/edit layer. */
void      Skink_ui_clipboard_set(const char *s) { SDL_SetClipboardText(s ? s : ""); }
char     *Skink_ui_clipboard_get(void) { char *t = SDL_GetClipboardText(); char *r = strdup(t ? t : ""); SDL_free(t); return r; }
long long Skink_ui_clipboard_has(void) { return SDL_HasClipboardText() ? 1 : 0; }

/* ------------------------------------------------------------------ */
/* Layout                                                              */
/* ------------------------------------------------------------------ */

void Skink_ui_row_dynamic(long long h, float height, int cols) { nk_layout_row_dynamic(ctxOf(h), height, cols); }
void Skink_ui_row_static(long long h, float height, int itemW, int cols) { nk_layout_row_static(ctxOf(h), height, (float)itemW, cols); }
void Skink_ui_row_begin(long long h, int isDynamic, float height, int cols) {
    nk_layout_row_begin(ctxOf(h), isDynamic ? NK_DYNAMIC : NK_STATIC, height, cols);
}
void Skink_ui_row_push(long long h, float v) { nk_layout_row_push(ctxOf(h), v); }
void Skink_ui_row_end(long long h) { nk_layout_row_end(ctxOf(h)); }
void Skink_ui_space_begin(long long h, int isDynamic, float height, int count) {
    nk_layout_space_begin(ctxOf(h), isDynamic ? NK_DYNAMIC : NK_STATIC, height, count);
}
void Skink_ui_space_push(long long h, float x, float y, float w, float ht) {
    nk_layout_space_push(ctxOf(h), nk_rect(x, y, w, ht));
}
void Skink_ui_space_end(long long h) { nk_layout_space_end(ctxOf(h)); }

/* Widget metrics — used by Skink-side hit-testing/bindings. */
void Skink_ui_widget_bounds(long long h, float *x, float *y, float *w, float *ht) {
    struct nk_rect r = nk_widget_bounds(ctxOf(h));
    *x = r.x; *y = r.y; *w = r.w; *ht = r.h;
}
long long Skink_ui_widget_is_hovered(long long h) {
    return nk_widget_is_hovered(ctxOf(h)) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Basic widgets — all return event state to Skink                     */
/* ------------------------------------------------------------------ */

void Skink_ui_label(long long h, const char *s, int align) {
    nk_label(ctxOf(h), s ? s : "", (nk_flags)(align ? NK_TEXT_RIGHT : NK_TEXT_LEFT));
}
void Skink_ui_label_colored(long long h, const char *s, int align, int r, int g, int b) {
    nk_label_colored(ctxOf(h), s ? s : "", (nk_flags)(align ? NK_TEXT_RIGHT : NK_TEXT_LEFT),
                     nk_rgb(r, g, b));
}
void Skink_ui_text_wrap(long long h, const char *s) { nk_text_wrap(ctxOf(h), s ? s : "", (int)strlen(s ? s : "")); }

long long Skink_ui_button(long long h, const char *s) {
    return nk_button_label(ctxOf(h), s ? s : "") ? 1 : 0;
}
long long Skink_ui_button_colored(long long h, int r, int g, int b) {
    return nk_button_color(ctxOf(h), nk_rgb(r, g, b)) ? 1 : 0;
}

/* Checkbox: returns the new checked state (in = current state). */
long long Skink_ui_checkbox(long long h, const char *label, long long active) {
    int v = (int)active;
    nk_checkbox_label(ctxOf(h), label ? label : "", &v);
    return v;
}

/* Radio: returns non-zero when this option is now selected. */
long long Skink_ui_radio(long long h, const char *label, long long active) {
    return nk_option_label(ctxOf(h), label ? label : "", (int)active) ? 1 : 0;
}

/* Selectable row (list items, menus). Returns new selected state. */
long long Skink_ui_selectable(long long h, const char *s, long long selected) {
    int v = (int)selected;
    nk_selectable_label(ctxOf(h), s ? s : "", NK_TEXT_LEFT, &v);
    return v;
}

long long Skink_ui_slide_int(long long h, long long mn, long long v, long long mx, long long step) {
    return nk_slide_int(ctxOf(h), (int)mn, (int)v, (int)mx, (int)step);
}
double Skink_ui_slide_float(long long h, double mn, double v, double mx, double step) {
    return nk_slide_float(ctxOf(h), (float)mn, (float)v, (float)mx, (float)step);
}

/* Progress: returns updated value (modifiable) or input (static). */
long long Skink_ui_progress(long long h, long long cur, long long max, long long modifiable) {
    nk_size v = (nk_size)cur;
    nk_progress(ctxOf(h), &v, (nk_size)max, (int)modifiable);
    return (long long)v;
}

/* Property box: int field with +/- spinners. Returns updated value. */
long long Skink_ui_property_int(long long h, const char *name, long long mn,
                                long long v, long long mx, long long step, double incPerPixel) {
    int iv = (int)v;
    nk_property_int(ctxOf(h), name ? name : "", (int)mn, &iv, (int)mx, (int)step, (float)incPerPixel);
    return iv;
}

/* ------------------------------------------------------------------ */
/* Text edit (per-widget buffer keyed by id)                           */
/* ------------------------------------------------------------------ */

/* flags bit0 = signal-on-enter. Returns NK_EDIT_* event mask. */
long long Skink_ui_edit(long long h, long long id, long long flags) {
    UiWin *w = winOf(h);
    UiEdit *e = editFor(w, (int)id);
    nk_flags ef = NK_EDIT_SIMPLE | NK_EDIT_CLIPBOARD;
    if (flags & 1) ef |= NK_EDIT_SIG_ENTER;
    return (long long)nk_edit_string(w->ctx, ef, e->text, &e->len,
                                     UI_EDIT_CAP - 1, nk_filter_default);
}

// Returned Skink strings must be RC-managed so the caller's release frees
// them — a plain strdup leaks under RC ownership (comp_error.md #104).
extern void *Skink_rc_alloc(int64_t payload_size);
char *Skink_ui_edit_text(long long h, long long id) {
    const char *s = editFor(winOf(h), (int)id)->text;
    size_t n = strlen(s) + 1;
    char *out = (char *)Skink_rc_alloc((int64_t)n);
    if (out)
        memcpy(out, s, n);
    return out;
}
void Skink_ui_edit_set(long long h, long long id, const char *s) {
    UiEdit *e = editFor(winOf(h), (int)id);
    int n = (int)(s ? strlen(s) : 0);
    if (n >= UI_EDIT_CAP) n = UI_EDIT_CAP - 1;
    memcpy(e->text, s ? s : "", n);
    e->text[n] = '\0';
    e->len = n;
}
long long Skink_ui_edit_active(long long h) {
    return nk_item_is_any_active(ctxOf(h)) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Combo boxes                                                         */
/* ------------------------------------------------------------------ */

long long Skink_ui_combo_begin(long long h, const char *selected, float w, float hh) {
    return nk_combo_begin_label(ctxOf(h), selected ? selected : "",
                                nk_vec2(w, hh)) ? 1 : 0;
}
long long Skink_ui_combo_item(long long h, const char *item) {
    return nk_combo_item_label(ctxOf(h), item ? item : "", NK_TEXT_LEFT) ? 1 : 0;
}
long long Skink_ui_combo_item_selected(long long h, const char *item) {
    /* selectable variant: also draws a highlight when chosen */
    return nk_combo_item_label(ctxOf(h), item ? item : "", NK_TEXT_LEFT) ? 1 : 0;
}
void Skink_ui_combo_end(long long h) { nk_combo_end(ctxOf(h)); }

/* ------------------------------------------------------------------ */
/* Scrollable groups & virtualized lists                               */
/* ------------------------------------------------------------------ */

long long Skink_ui_group_begin(long long h, const char *title, long long scrollable) {
    return nk_group_begin(ctxOf(h), title ? title : "",
                          scrollable ? NK_WINDOW_BORDER : 0) ? 1 : 0;
}
void Skink_ui_group_end(long long h) { nk_group_end(ctxOf(h)); }

/* list_begin sets up a virtualized list; rows [first,last) are drawn by
 * Skink via list_item. Height is the list's total pixel height. */
long long Skink_ui_list_begin(long long h, float height, int rowH, int count) {
    UiWin *w = winOf(h);
    if (w->lvActive) return 0;
    if (nk_list_view_begin(w->ctx, &w->lv, "lv", 0, rowH, count)) {
        w->lvActive = 1;
        return 1;
    }
    return 0;
}
long long Skink_ui_list_first(long long h) { return winOf(h)->lv.begin; }
long long Skink_ui_list_last(long long h)  { return winOf(h)->lv.end; }
long long Skink_ui_list_item(long long h, const char *s, long long selected) {
    int v = (int)selected;
    nk_selectable_label(ctxOf(h), s ? s : "", NK_TEXT_LEFT, &v);
    return v;
}
void Skink_ui_list_end(long long h) {
    UiWin *w = winOf(h);
    if (w->lvActive) { nk_list_view_end(&w->lv); w->lvActive = 0; }
}

/* ------------------------------------------------------------------ */
/* Trees, menus, popups, tabs-strip helpers, color picker              */
/* ------------------------------------------------------------------ */

long long Skink_ui_tree_begin(long long h, long long id, const char *title, long long node) {
    UiWin *w = winOf(h);
    enum nk_collapse_states *st = treeStateFor(w, (int)id);
    return nk_tree_state_push(w->ctx, node ? NK_TREE_NODE : NK_TREE_TAB,
                              title ? title : "", st) ? 1 : 0;
}
void Skink_ui_tree_pop(long long h) { nk_tree_pop(ctxOf(h)); }

void Skink_ui_menubar_begin(long long h) { nk_menubar_begin(ctxOf(h)); }
void Skink_ui_menubar_end(long long h) { nk_menubar_end(ctxOf(h)); }
long long Skink_ui_menu_begin(long long h, const char *label, float w, float hh) {
    return nk_menu_begin_label(ctxOf(h), label ? label : "", NK_TEXT_LEFT,
                               nk_vec2(w, hh)) ? 1 : 0;
}
long long Skink_ui_menu_item(long long h, const char *label) {
    return nk_menu_item_label(ctxOf(h), label ? label : "", NK_TEXT_LEFT) ? 1 : 0;
}
void Skink_ui_menu_end(long long h) { nk_menu_end(ctxOf(h)); }

long long Skink_ui_popup_begin(long long h, const char *title, float x, float y, float w, float hh) {
    return nk_popup_begin(ctxOf(h), NK_POPUP_STATIC, title ? title : "", 0,
                          nk_rect(x, y, w, hh)) ? 1 : 0;
}
void Skink_ui_popup_close(long long h) { nk_popup_close(ctxOf(h)); }
void Skink_ui_popup_end(long long h) { nk_popup_end(ctxOf(h)); }

void Skink_ui_tooltip(long long h, const char *s) { nk_tooltip(ctxOf(h), s ? s : ""); }

/* Color picker: rgba packed 0xRRGGBBAA; returns packed updated value. */
long long Skink_ui_color_picker(long long h, long long rgba, long long withAlpha) {
    struct nk_colorf c;
    c.r = (float)((rgba >> 24) & 0xFF) / 255.0f;
    c.g = (float)((rgba >> 16) & 0xFF) / 255.0f;
    c.b = (float)((rgba >> 8)  & 0xFF) / 255.0f;
    c.a = (float)(rgba & 0xFF) / 255.0f;
    c = nk_color_picker(ctxOf(h), c, withAlpha ? NK_RGBA : NK_RGB);
    return ((long long)(c.r * 255) << 24) | ((long long)(c.g * 255) << 16) |
           ((long long)(c.b * 255) << 8)  | (long long)(c.a * 255);
}

/* Input queries for Skink-side dispatch (drag widgets, custom hit tests). */
long long Skink_ui_mouse_x(long long h) { return (long long)ctxOf(h)->input.mouse.pos.x; }
long long Skink_ui_mouse_y(long long h) { return (long long)ctxOf(h)->input.mouse.pos.y; }
long long Skink_ui_mouse_down(long long h, long long btn) {
    return nk_input_is_mouse_down(&ctxOf(h)->input, (enum nk_buttons)btn) ? 1 : 0;
}
long long Skink_ui_mouse_clicked(long long h, long long btn) {
    return nk_input_is_mouse_click_in_rect(&ctxOf(h)->input, (enum nk_buttons)btn,
                                           nk_window_get_bounds(ctxOf(h))) ? 1 : 0;
}
