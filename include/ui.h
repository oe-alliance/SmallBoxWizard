#ifndef SMALLBOX_UI_H
#define SMALLBOX_UI_H

#include <linux/fb.h>
#include <stddef.h>
#include <stdint.h>

#define UI_MAX_ITEMS 12
#define UI_GREEN "\001"  /* The colors of a title after its tab. */
#define UI_RED "\002"

/* Font Awesome icons of the fonts, see tools/mkfonts.sh. */
#define UI_ICON_WELCOME "\xEF\x80\x95"  /* U+F015 house */
#define UI_ICON_MODE "\xEF\x87\x9E"  /* U+F1DE sliders */
#define UI_ICON_STORAGE "\xEF\x82\xA0"  /* U+F0A0 hard-drive */
#define UI_ICON_NETWORK "\xEF\x9B\xBF"  /* U+F6FF network-wired */
#define UI_ICON_INSTALL "\xEF\x80\x99"  /* U+F019 download */
#define UI_ICON_FINISH "\xEF\x84\x9E"  /* U+F11E flag-checkered */
#define UI_ICON_DONE "\xEF\x80\x8C"  /* U+F00C check */
#define UI_ICON_INFO "\xEF\x81\x9A"  /* U+F05A circle-info */

struct ui_color {
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t a;
};

struct ui_context {
	int fd;
	uint8_t *screen;  /* The framebuffer, LVGL draws into it. */
	size_t memory_size;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	int manual_blit;
	char device[32];
};

/* The screen is split: the menu of ui_sidebar on the left, the screen of its entry on the right,
 * which every other call below draws. */
int ui_open(struct ui_context *ui);
void ui_close(struct ui_context *ui);
void ui_clear(struct ui_context *ui, struct ui_color color);
void ui_present(struct ui_context *ui);
/* The steps on the left; marks[i] 1 shows step i as done with a green check, 2 greys it out
 * (NULL: none). */
void ui_sidebar(const struct ui_context *ui, const char *const names[], const char *const icons[], int count,
	const char *marks);
/* The chosen entry, bright while the menu has the keys, dim while the screen on the right has them. */
void ui_sidebar_select(const struct ui_context *ui, int selected, int focused);
/* Moves the keys between the menu and the screen, returns where they were. */
int ui_sidebar_focus(const struct ui_context *ui, int focused);
/* What an entry of the menu does: a card above the title (card NULL: none, warn: in amber), the
 * body and info below it in grey. */
struct ui_card_page {
	const char *title;
	const char *card_title;
	const char *card;
	int warn;
	const char *body;
	const char *info;
	const char *footer;
};
void ui_preview(const struct ui_context *ui, const struct ui_card_page *page);
void ui_screen(const struct ui_context *ui, const char *title, const char *body,
	const char *footer);
void ui_menu(const struct ui_context *ui, const char *title, const char *body,
	const char *const items[], int item_count, int selected,
	const char *footer);
/* A menu for the calls below, each uses only some fields. marked is -1 for none, so set it. */
struct ui_menu {
	const char *title;
	const char *body;
	const char *warning;  /* In an amber card above the body, NULL for none. */
	const char *header;
	const char *const *items;
	int count;
	int selected;
	const char *marks;
	int marked;
	const char *align;
	const char *footer;
};
/* Like ui_menu, the item marked (or -1) is the current one and says so. */
void ui_menu_marked(const struct ui_context *ui, const struct ui_menu *menu);
/* A menu with columns like ui_text below header, marks[i] 1 draws item i in
 * yellow, 2 in grey (NULL: only marked in yellow). */
void ui_menu_table(const struct ui_context *ui, const struct ui_menu *menu);
/* The rows of the menu drawn last that fit on the screen. */
int ui_menu_rows(void);
/* The bytes at the start of text that fit on one line of ui_text, all without a screen. */
size_t ui_text_fit(const char *text);
void ui_progress(const struct ui_context *ui, const char *title, const char *body,
	int percent, const char *detail, const char *footer);
void ui_error(const struct ui_context *ui, const char *title, const char *message);
/* Like ui_error with the keys of ui_keys in the footer. */
void ui_error_keys(const struct ui_context *ui, const char *title, const char *message, const char *footer_keys);
void ui_redraw(const struct ui_context *ui);
/* A spinner and the running time also when the footer offers keys, e.g. "BACK: Cancel" of a download. Without
 * keys in the footer they are shown anyway. */
void ui_busy(const struct ui_context *ui, int on);
/* 1 when the footer drawn last offers OK, so nothing runs that a key could interrupt. */
int ui_offers_ok(void);
/* Lights up the key of the footer, named like "OK" or "UP", for a moment. */
void ui_key_pressed(const char *key);
/* The screen without a window of width x height in the middle, which another program draws. */
void ui_embed(const struct ui_context *ui, const char *title, const char *footer, int width, int height);
/* 1 for a screen of at least 1280 pixels, 0 for SD like 720 x 576. */
int ui_wide(void);
/* A note in the middle of the header, e.g. "Demo mode"; the clock is on its right. */
void ui_header(const struct ui_context *ui, const char *text);
/* Draws the header again when its clock is a minute behind. */
void ui_clock(const struct ui_context *ui);
/* A card above every screen on the right with "Name	value" lines side by side, "" for none. */
void ui_summary(const struct ui_context *ui, const char *text);
/* On while a dialog covers the screen, off shows the screen again. */
void ui_overlay(const struct ui_context *ui, int on);
/* A tab in a title draws the rest of it in yellow, in green or red after UI_GREEN or UI_RED, and
 * a second tab the rest on the right; in a footer a tab aligns the rest on the right.
 * The keys of a screen below it, always in the order ARROWS, digits, OK, RED,
 * GREEN, YELLOW, BLUE, BACK; NULL leaves a key out. digit_keys names the
 * digits, e.g. "1-9". A footer part without a key, like "Please wait...", shows in yellow. */
struct ui_key_names {
	const char *arrows;
	const char *digit_keys;
	const char *digits;
	const char *ok;
	const char *red;
	const char *green;
	const char *yellow;
	const char *blue;
	const char *back;
};
void ui_keys(char *footer, size_t size, const struct ui_key_names *keys);

/* Lines from first on, a tab separates columns as wide as their widest text.
 * align has 'l' or 'r' per column, NULL for left; the last column is cut at
 * the edge. A scrollbar shows when not all lines fit. Returns the rows that fit. */
struct ui_text_page {
	const char *title;
	const char *header;
	const char *const *lines;
	int count;
	int first;
	const char *align;
	const char *footer;
};
int ui_text(const struct ui_context *ui, const struct ui_text_page *page);

#endif
