#define _GNU_SOURCE

#include "viewer.h"

#include <stdio.h>

enum input_key text_view(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop, const struct text_page *page)
{
	const char *const nothing[] = {page->empty};
	int count = page->count;
	int *first = page->first;
	while (!(stop && *stop)) {
		enum input_key key;
		int rows = ui_text(ui, &(struct ui_text_page){.title = page->title, .header = count ? page->header : NULL,
			.lines = count ? (const char *const *)page->lines : nothing, .count = count ? count : 1,
			.first = *first, .align = page->align, .footer = page->footer});
		int last = count > rows ? count - rows : 0;
		key = input_next(input, 1000);
		if (key == INPUT_UP)
			(*first)--;
		else if (key == INPUT_DOWN)
			(*first)++;
		else if (key == INPUT_LEFT)
			*first -= rows;
		else if (key == INPUT_RIGHT)
			*first += rows;
		else if (key != INPUT_NONE)
			return key;
		if (*first > last)
			*first = last;
		if (*first < 0)
			*first = 0;
	}
	return INPUT_NONE;
}

int list_move(enum input_key key, int selected, int count)
{
	int page = ui_menu_rows();
	if (count <= 0)
		return selected;
	if (key == INPUT_UP)
		return (selected + count - 1) % count;
	if (key == INPUT_DOWN)
		return (selected + 1) % count;
	if (key == INPUT_LEFT)
		return selected > page ? selected - page : 0;
	if (key == INPUT_RIGHT)
		return selected + page < count ? selected + page : count - 1;
	return selected;
}

int ask(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop, const char *title,
	const char *question, int yes)
{
	const char *items[2];
	char footer[96];
	int selected = yes ? 0 : 1;
	int answer = 0;
	ui_overlay(ui, 1);
	while (!(stop && *stop)) {
		enum input_key key;
		items[0] = "Yes";
		items[1] = "No";
		ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = "Select", .ok = "Confirm"});
		ui_menu(ui, title, question, items, 2, selected, footer);
		key = input_next(input, 1000);
		selected = list_move(key, selected, 2);
		if (key == INPUT_OK || key == INPUT_BACK) {
			answer = key == INPUT_OK && selected == 0;
			break;
		}
	}
	ui_overlay(ui, 0);
	return answer;
}
