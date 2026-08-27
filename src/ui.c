#define _GNU_SOURCE

#include "ui.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef FBIO_SET_MANUAL_BLIT
#define FBIO_SET_MANUAL_BLIT _IOW('F', 0x21, __u8)
#endif
#ifndef FBIO_BLIT
#define FBIO_BLIT 0x22
#endif

#define FONT_FIRST 32
#define FONT_LAST 90
#define FONT_LINE_ADVANCE 10

/* Compact 5x7 ASCII font. Lowercase is deliberately rendered as uppercase;
 * that keeps the binary small and avoids any runtime font dependency. */
static const uint8_t glyphs[FONT_LAST - FONT_FIRST + 1][7] = {
	[' ' - FONT_FIRST] = {0, 0, 0, 0, 0, 0, 0},
	['!' - FONT_FIRST] = {4, 4, 4, 4, 4, 0, 4},
	['"' - FONT_FIRST] = {10, 10, 10, 0, 0, 0, 0},
	['#' - FONT_FIRST] = {10, 31, 10, 10, 31, 10, 0},
	['$' - FONT_FIRST] = {4, 15, 20, 14, 5, 30, 4},
	['%' - FONT_FIRST] = {25, 26, 2, 4, 8, 11, 19},
	['&' - FONT_FIRST] = {12, 18, 20, 8, 21, 18, 13},
	['\'' - FONT_FIRST] = {4, 4, 8, 0, 0, 0, 0},
	['(' - FONT_FIRST] = {2, 4, 8, 8, 8, 4, 2},
	[')' - FONT_FIRST] = {8, 4, 2, 2, 2, 4, 8},
	['*' - FONT_FIRST] = {0, 4, 21, 14, 21, 4, 0},
	['+' - FONT_FIRST] = {0, 4, 4, 31, 4, 4, 0},
	[',' - FONT_FIRST] = {0, 0, 0, 0, 4, 4, 8},
	['-' - FONT_FIRST] = {0, 0, 0, 31, 0, 0, 0},
	['.' - FONT_FIRST] = {0, 0, 0, 0, 0, 12, 12},
	['/' - FONT_FIRST] = {1, 2, 2, 4, 8, 8, 16},
	['0' - FONT_FIRST] = {14, 17, 19, 21, 25, 17, 14},
	['1' - FONT_FIRST] = {4, 12, 4, 4, 4, 4, 14},
	['2' - FONT_FIRST] = {14, 17, 1, 2, 4, 8, 31},
	['3' - FONT_FIRST] = {30, 1, 1, 14, 1, 1, 30},
	['4' - FONT_FIRST] = {2, 6, 10, 18, 31, 2, 2},
	['5' - FONT_FIRST] = {31, 16, 16, 30, 1, 1, 30},
	['6' - FONT_FIRST] = {6, 8, 16, 30, 17, 17, 14},
	['7' - FONT_FIRST] = {31, 1, 2, 4, 8, 8, 8},
	['8' - FONT_FIRST] = {14, 17, 17, 14, 17, 17, 14},
	['9' - FONT_FIRST] = {14, 17, 17, 15, 1, 2, 12},
	[':' - FONT_FIRST] = {0, 12, 12, 0, 12, 12, 0},
	[';' - FONT_FIRST] = {0, 12, 12, 0, 4, 4, 8},
	['<' - FONT_FIRST] = {2, 4, 8, 16, 8, 4, 2},
	['=' - FONT_FIRST] = {0, 0, 31, 0, 31, 0, 0},
	['>' - FONT_FIRST] = {8, 4, 2, 1, 2, 4, 8},
	['?' - FONT_FIRST] = {14, 17, 1, 2, 4, 0, 4},
	['@' - FONT_FIRST] = {14, 17, 23, 21, 23, 16, 14},
	['A' - FONT_FIRST] = {14, 17, 17, 31, 17, 17, 17},
	['B' - FONT_FIRST] = {30, 17, 17, 30, 17, 17, 30},
	['C' - FONT_FIRST] = {14, 17, 16, 16, 16, 17, 14},
	['D' - FONT_FIRST] = {28, 18, 17, 17, 17, 18, 28},
	['E' - FONT_FIRST] = {31, 16, 16, 30, 16, 16, 31},
	['F' - FONT_FIRST] = {31, 16, 16, 30, 16, 16, 16},
	['G' - FONT_FIRST] = {14, 17, 16, 23, 17, 17, 15},
	['H' - FONT_FIRST] = {17, 17, 17, 31, 17, 17, 17},
	['I' - FONT_FIRST] = {14, 4, 4, 4, 4, 4, 14},
	['J' - FONT_FIRST] = {7, 2, 2, 2, 2, 18, 12},
	['K' - FONT_FIRST] = {17, 18, 20, 24, 20, 18, 17},
	['L' - FONT_FIRST] = {16, 16, 16, 16, 16, 16, 31},
	['M' - FONT_FIRST] = {17, 27, 21, 21, 17, 17, 17},
	['N' - FONT_FIRST] = {17, 25, 21, 19, 17, 17, 17},
	['O' - FONT_FIRST] = {14, 17, 17, 17, 17, 17, 14},
	['P' - FONT_FIRST] = {30, 17, 17, 30, 16, 16, 16},
	['Q' - FONT_FIRST] = {14, 17, 17, 17, 21, 18, 13},
	['R' - FONT_FIRST] = {30, 17, 17, 30, 20, 18, 17},
	['S' - FONT_FIRST] = {15, 16, 16, 14, 1, 1, 30},
	['T' - FONT_FIRST] = {31, 4, 4, 4, 4, 4, 4},
	['U' - FONT_FIRST] = {17, 17, 17, 17, 17, 17, 14},
	['V' - FONT_FIRST] = {17, 17, 17, 17, 17, 10, 4},
	['W' - FONT_FIRST] = {17, 17, 17, 21, 21, 21, 10},
	['X' - FONT_FIRST] = {17, 17, 10, 4, 10, 17, 17},
	['Y' - FONT_FIRST] = {17, 17, 10, 4, 4, 4, 4},
	['Z' - FONT_FIRST] = {31, 1, 2, 4, 8, 16, 31}
};

static const struct ui_color COLOR_BACKGROUND = {10, 20, 31, 255};
static const struct ui_color COLOR_PANEL = {25, 39, 54, 255};
static const struct ui_color COLOR_HEADER = {0, 133, 194, 255};
static const struct ui_color COLOR_TEXT = {235, 241, 245, 255};
static const struct ui_color COLOR_MUTED = {167, 183, 196, 255};
static const struct ui_color COLOR_SELECT = {0, 174, 239, 255};
static const struct ui_color COLOR_SELECT_TEXT = {3, 22, 34, 255};
static const struct ui_color COLOR_ERROR = {187, 45, 45, 255};

static uint32_t scale_channel(uint8_t value, uint32_t length)
{
	uint64_t maximum;
	if (length == 0)
		return 0;
	if (length >= 32)
		return value;
	maximum = ((uint64_t)1U << length) - 1U;
	return (uint32_t)(((uint64_t)value * maximum + 127U) / 255U);
}

static uint32_t pack_pixel(const struct ui_context *ui, struct ui_color color)
{
	uint32_t pixel = 0;
	pixel |= scale_channel(color.r, ui->var.red.length) << ui->var.red.offset;
	pixel |= scale_channel(color.g, ui->var.green.length) << ui->var.green.offset;
	pixel |= scale_channel(color.b, ui->var.blue.length) << ui->var.blue.offset;
	if (ui->var.transp.length)
		pixel |= scale_channel(color.a, ui->var.transp.length) <<
			ui->var.transp.offset;
	return pixel;
}

static void fill_rect(struct ui_context *ui, int x, int y, int width,
	int height, struct ui_color color)
{
	uint8_t packed[4];
	uint32_t pixel;
	unsigned int bytes;
	int row;
	int column;
	int x2 = x + width;
	int y2 = y + height;
	if (!ui || !ui->memory || width <= 0 || height <= 0)
		return;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x2 > (int)ui->var.xres) x2 = (int)ui->var.xres;
	if (y2 > (int)ui->var.yres) y2 = (int)ui->var.yres;
	if (x >= x2 || y >= y2)
		return;
	bytes = (ui->var.bits_per_pixel + 7U) / 8U;
	pixel = pack_pixel(ui, color);
	if (bytes == 2 || bytes == 4) {
		memcpy(packed, &pixel, bytes);
	} else {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
		packed[0] = (uint8_t)pixel;
		packed[1] = (uint8_t)(pixel >> 8);
		packed[2] = (uint8_t)(pixel >> 16);
#else
		packed[0] = (uint8_t)(pixel >> 16);
		packed[1] = (uint8_t)(pixel >> 8);
		packed[2] = (uint8_t)pixel;
#endif
	}
	for (row = y; row < y2; ++row) {
		size_t offset = (size_t)(row + (int)ui->var.yoffset) *
			ui->fix.line_length +
			(size_t)(x + (int)ui->var.xoffset) * bytes;
		for (column = x; column < x2; ++column) {
			if (offset + bytes > ui->memory_size)
				break;
			memcpy(ui->memory + offset, packed, bytes);
			offset += bytes;
		}
	}
}

static char display_char(char ch)
{
	if (ch >= 'a' && ch <= 'z')
		ch = (char)(ch - 'a' + 'A');
	if (ch < FONT_FIRST || ch > FONT_LAST)
		return '?';
	return ch;
}

static void draw_char(struct ui_context *ui, int x, int y, char ch,
	int scale, struct ui_color color)
{
	const uint8_t *glyph;
	int row;
	int column;
	ch = display_char(ch);
	glyph = glyphs[(unsigned char)ch - FONT_FIRST];
	for (row = 0; row < 7; ++row) {
		for (column = 0; column < 5; ++column) {
			if (glyph[row] & (1U << (4 - column)))
				fill_rect(ui, x + column * scale, y + row * scale,
					scale, scale, color);
		}
	}
}

static void draw_text(struct ui_context *ui, int x, int y, const char *text,
	int scale, struct ui_color color)
{
	int cursor = x;
	if (!ui || !text)
		return;
	while (*text) {
		if (*text == '\n') {
			y += FONT_LINE_ADVANCE * scale;
			cursor = x;
		} else {
			draw_char(ui, cursor, y, *text, scale, color);
			cursor += 6 * scale;
		}
		text++;
	}
}

static int draw_wrapped(struct ui_context *ui, int x, int y, int width,
	const char *text, int scale, struct ui_color color, int max_lines)
{
	char line[192];
	int capacity = width / (6 * scale);
	int line_length = 0;
	int lines = 0;
	const char *cursor = text ? text : "";

	if (capacity < 8)
		capacity = 8;
	while (*cursor && lines < max_lines) {
		const char *word;
		int word_length;
		while (*cursor == ' ')
			cursor++;
		if (*cursor == '\n') {
			line[line_length] = '\0';
			draw_text(ui, x, y + lines * FONT_LINE_ADVANCE * scale,
				line, scale, color);
			line_length = 0;
			lines++;
			cursor++;
			continue;
		}
		if (!*cursor)
			break;
		word = cursor;
		while (*cursor && *cursor != ' ' && *cursor != '\n')
			cursor++;
		word_length = (int)(cursor - word);
		if (line_length && line_length + 1 + word_length > capacity) {
			line[line_length] = '\0';
			draw_text(ui, x, y + lines * FONT_LINE_ADVANCE * scale,
				line, scale, color);
			line_length = 0;
			lines++;
			if (lines >= max_lines)
				break;
		}
		if (line_length)
			line[line_length++] = ' ';
		while (word_length > 0 && line_length < capacity &&
			line_length + 1 < (int)sizeof(line)) {
			line[line_length++] = *word++;
			word_length--;
		}
	}
	if (line_length && lines < max_lines) {
		line[line_length] = '\0';
		draw_text(ui, x, y + lines * FONT_LINE_ADVANCE * scale,
			line, scale, color);
		lines++;
	}
	return lines * FONT_LINE_ADVANCE * scale;
}

int ui_open(struct ui_context *ui)
{
	static const char *const devices[] = {
		"/dev/fb0", "/dev/fb/0", "/dev/fb1", NULL
	};
	int i;
	unsigned char manual = 1;

	if (!ui)
		return 0;
	memset(ui, 0, sizeof(*ui));
	ui->fd = -1;
	for (i = 0; devices[i]; ++i) {
		ui->fd = open(devices[i], O_RDWR | O_CLOEXEC);
		if (ui->fd >= 0) {
			snprintf(ui->device, sizeof(ui->device), "%s", devices[i]);
			break;
		}
	}
	if (ui->fd < 0)
		return 0;
	if (ioctl(ui->fd, FBIOGET_VSCREENINFO, &ui->var) < 0 ||
		ioctl(ui->fd, FBIOGET_FSCREENINFO, &ui->fix) < 0)
		goto failed;
	if (ui->var.bits_per_pixel != 16 && ui->var.bits_per_pixel != 24 &&
		ui->var.bits_per_pixel != 32) {
		errno = ENOTSUP;
		goto failed;
	}
	ui->memory_size = ui->fix.smem_len;
	if (!ui->memory_size)
		ui->memory_size = (size_t)ui->fix.line_length * ui->var.yres_virtual;
	ui->memory = mmap(NULL, ui->memory_size, PROT_READ | PROT_WRITE,
		MAP_SHARED, ui->fd, 0);
	if (ui->memory == MAP_FAILED) {
		ui->memory = NULL;
		goto failed;
	}
	if (ioctl(ui->fd, FBIO_SET_MANUAL_BLIT, &manual) == 0)
		ui->manual_blit = 1;
	ui_clear(ui, COLOR_BACKGROUND);
	ui_present(ui);
	return 1;

failed:
	if (ui->fd >= 0)
		close(ui->fd);
	ui->fd = -1;
	return 0;
}

void ui_close(struct ui_context *ui)
{
	unsigned char manual = 0;
	if (!ui)
		return;
	if (ui->memory) {
		msync(ui->memory, ui->memory_size, MS_SYNC);
		munmap(ui->memory, ui->memory_size);
		ui->memory = NULL;
	}
	if (ui->fd >= 0) {
		if (ui->manual_blit)
			ioctl(ui->fd, FBIO_SET_MANUAL_BLIT, &manual);
		close(ui->fd);
		ui->fd = -1;
	}
}

void ui_clear(struct ui_context *ui, struct ui_color color)
{
	if (ui && ui->memory)
		fill_rect(ui, 0, 0, (int)ui->var.xres, (int)ui->var.yres, color);
}

void ui_present(struct ui_context *ui)
{
	if (!ui || ui->fd < 0)
		return;
	if (ui->manual_blit)
		ioctl(ui->fd, FBIO_BLIT);
	msync(ui->memory, ui->memory_size, MS_ASYNC);
}

static void panel_geometry(const struct ui_context *ui, int *x, int *y,
	int *width, int *height)
{
	*width = (int)ui->var.xres * 9 / 10;
	*height = (int)ui->var.yres * 17 / 20;
	if (*width < 520) *width = (int)ui->var.xres - 20;
	if (*height < 360) *height = (int)ui->var.yres - 20;
	*x = ((int)ui->var.xres - *width) / 2;
	*y = ((int)ui->var.yres - *height) / 2;
}

static int body_scale(const struct ui_context *ui)
{
	return ui->var.xres >= 1000 ? 3 : 2;
}

static int prominent_text_scale(const struct ui_context *ui)
{
	if (ui->var.xres >= 1000 && ui->var.yres >= 650)
		return 4;
	if (ui->var.xres >= 640 && ui->var.yres >= 480)
		return 3;
	return 2;
}

static void draw_frame(struct ui_context *ui, const char *title,
	const char *footer, int *content_x, int *content_y,
	int *content_width, int *content_height)
{
	int x, y, width, height;
	int margin;
	int scale = body_scale(ui);
	int header_height = 13 * scale;
	int footer_height = 12 * scale;
	panel_geometry(ui, &x, &y, &width, &height);
	margin = 6 * scale;
	ui_clear(ui, COLOR_BACKGROUND);
	fill_rect(ui, x, y, width, height, COLOR_PANEL);
	fill_rect(ui, x, y, width, header_height, COLOR_HEADER);
	draw_text(ui, x + margin, y + 3 * scale,
		title ? title : "OE-ALLIANCE SMALLBOX WIZARD", scale, COLOR_TEXT);
	fill_rect(ui, x, y + height - footer_height, width, footer_height,
		COLOR_BACKGROUND);
	draw_text(ui, x + margin, y + height - footer_height + 3 * scale,
		footer ? footer : "OK: CONTINUE", scale, COLOR_MUTED);
	*content_x = x + margin;
	*content_y = y + header_height + margin;
	*content_width = width - margin * 2;
	*content_height = height - header_height - footer_height - margin * 2;
}

void ui_screen(struct ui_context *ui, const char *title, const char *body,
	const char *footer)
{
	int x, y, width, height;
	int scale;
	int max_lines;
	int used;
	if (!ui || !ui->memory)
		return;
	draw_frame(ui, title, footer, &x, &y, &width, &height);
	scale = prominent_text_scale(ui);
	max_lines = height / (FONT_LINE_ADVANCE * scale);
	if (max_lines < 1)
		max_lines = 1;
	if (max_lines > 14)
		max_lines = 14;
	used = draw_wrapped(NULL, x, y, width, body, scale, COLOR_TEXT, max_lines);
	if (used < height)
		y += (height - used) / 2;
	draw_wrapped(ui, x, y, width, body, scale, COLOR_TEXT, max_lines);
	ui_present(ui);
}

void ui_menu(struct ui_context *ui, const char *title, const char *body,
	const char *const items[], int item_count, int selected,
	const char *footer)
{
	int x, y, width, height;
	int item_scale;
	int text_scale;
	int used;
	int row_height;
	int max_body_lines;
	int content_bottom;
	int reserved_height;
	int items_height;
	int group_height;
	int i;
	if (!ui || !ui->memory)
		return;
	item_scale = body_scale(ui);
	text_scale = prominent_text_scale(ui);
	draw_frame(ui, title, footer, &x, &y, &width, &height);
	row_height = 11 * item_scale;
	content_bottom = y + height;
	items_height = item_count * row_height;
	if (item_count > 1)
		items_height += (item_count - 1) * item_scale;
	reserved_height = items_height + 3 * item_scale;
	max_body_lines = (height - reserved_height) /
		(FONT_LINE_ADVANCE * text_scale);
	if (max_body_lines < 1)
		max_body_lines = 1;
	if (max_body_lines > 14)
		max_body_lines = 14;
	used = draw_wrapped(NULL, x, y, width, body, text_scale, COLOR_TEXT,
		max_body_lines);
	group_height = used + 3 * item_scale + items_height;
	if (group_height < height)
		y += (height - group_height) / 2;
	draw_wrapped(ui, x, y, width, body, text_scale, COLOR_TEXT,
		max_body_lines);
	y += used + 3 * item_scale;
	for (i = 0; i < item_count && y + row_height <=
		content_bottom; ++i) {
		struct ui_color text_color = COLOR_TEXT;
		if (i == selected) {
			fill_rect(ui, x, y, width, row_height, COLOR_SELECT);
			text_color = COLOR_SELECT_TEXT;
		}
		draw_text(ui, x + 3 * item_scale, y + 2 * item_scale,
			items[i] ? items[i] : "", item_scale, text_color);
		y += row_height + item_scale;
	}
	ui_present(ui);
}

void ui_progress(struct ui_context *ui, const char *title, const char *body,
	int percent, const char *detail, const char *footer)
{
	int x, y, width, height;
	int frame_scale;
	int text_scale;
	int body_height;
	int detail_height;
	int detail_lines;
	int group_height;
	int bar_height;
	if (!ui || !ui->memory)
		return;
	if (percent < 0) percent = 0;
	if (percent > 100) percent = 100;
	frame_scale = body_scale(ui);
	text_scale = prominent_text_scale(ui);
	draw_frame(ui, title, footer, &x, &y, &width, &height);
	body_height = draw_wrapped(NULL, x, y, width, body, text_scale,
		COLOR_TEXT, 4);
	bar_height = 8 * frame_scale;
	detail_lines = (height - body_height - bar_height -
		10 * frame_scale) / (FONT_LINE_ADVANCE * text_scale);
	if (detail_lines < 1)
		detail_lines = 1;
	if (detail_lines > 6)
		detail_lines = 6;
	detail_height = draw_wrapped(NULL, x, y, width,
		detail ? detail : "", text_scale, COLOR_MUTED, detail_lines);
	group_height = body_height + 5 * frame_scale + bar_height +
		5 * frame_scale + detail_height;
	if (group_height < height)
		y += (height - group_height) / 2;
	draw_wrapped(ui, x, y, width, body, text_scale, COLOR_TEXT, 4);
	y += body_height + 5 * frame_scale;
	fill_rect(ui, x, y, width, bar_height, COLOR_BACKGROUND);
	fill_rect(ui, x + frame_scale, y + frame_scale,
		(width - 2 * frame_scale) * percent / 100,
		bar_height - 2 * frame_scale, COLOR_SELECT);
	y += bar_height + 5 * frame_scale;
	draw_wrapped(ui, x, y, width, detail ? detail : "", text_scale,
		COLOR_MUTED, detail_lines);
	ui_present(ui);
}

void ui_error(struct ui_context *ui, const char *title, const char *message)
{
	int x, y, width, height;
	int frame_scale;
	int text_scale;
	int message_height;
	int max_lines;
	int group_height;
	int bar_height;
	if (!ui || !ui->memory)
		return;
	frame_scale = body_scale(ui);
	text_scale = prominent_text_scale(ui);
	draw_frame(ui, title ? title : "ERROR", "OK: BACK",
		&x, &y, &width, &height);
	bar_height = 10 * frame_scale;
	max_lines = (height - bar_height - 4 * frame_scale) /
		(FONT_LINE_ADVANCE * text_scale);
	if (max_lines < 1)
		max_lines = 1;
	if (max_lines > 12)
		max_lines = 12;
	message_height = draw_wrapped(NULL, x, y, width, message, text_scale,
		COLOR_TEXT, max_lines);
	group_height = bar_height + 4 * frame_scale + message_height;
	if (group_height < height)
		y += (height - group_height) / 2;
	fill_rect(ui, x, y, width, bar_height, COLOR_ERROR);
	draw_text(ui, x + 3 * frame_scale, y + 2 * frame_scale, "ERROR",
		frame_scale, COLOR_TEXT);
	draw_wrapped(ui, x, y + bar_height + 4 * frame_scale, width, message,
		text_scale, COLOR_TEXT, max_lines);
	ui_present(ui);
}
