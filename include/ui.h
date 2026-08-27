#ifndef SMALLBOX_UI_H
#define SMALLBOX_UI_H

#include <linux/fb.h>
#include <stddef.h>
#include <stdint.h>

#define UI_MAX_ITEMS 12

struct ui_color {
	uint8_t r;
	uint8_t g;
	uint8_t b;
	uint8_t a;
};

struct ui_context {
	int fd;
	uint8_t *memory;
	size_t memory_size;
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	int manual_blit;
	char device[32];
};

int ui_open(struct ui_context *ui);
void ui_close(struct ui_context *ui);
void ui_clear(struct ui_context *ui, struct ui_color color);
void ui_present(struct ui_context *ui);
void ui_screen(struct ui_context *ui, const char *title, const char *body,
	const char *footer);
void ui_menu(struct ui_context *ui, const char *title, const char *body,
	const char *const items[], int item_count, int selected,
	const char *footer);
void ui_progress(struct ui_context *ui, const char *title, const char *body,
	int percent, const char *detail, const char *footer);
void ui_error(struct ui_context *ui, const char *title, const char *message);

#endif
