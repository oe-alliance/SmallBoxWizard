#ifndef SMALLBOX_VIEWER_H
#define SMALLBOX_VIEWER_H

#include <signal.h>

#include "input.h"
#include "ui.h"

/* Lines like ui_text from *first on, empty shows when there are none. */
struct text_page {
	const char *title;
	const char *header;
	char *const *lines;
	int count;
	int *first;
	const char *align;
	const char *empty;
	const char *footer;
};

/* Scrolls the lines of page until a key that is no scroll key, which it returns. */
enum input_key text_view(const struct ui_context *ui, struct input_context *input,
	const volatile sig_atomic_t *stop, const struct text_page *page);

/* UP and DOWN move one item and wrap, LEFT and RIGHT a page of the menu drawn last and stop at
 * its ends; other keys keep selected. */
int list_move(enum input_key key, int selected, int count);

/* Yes or No in a dialog over the screen, yes chosen at first or no; BACK is no. Returns 1 for yes. */
int ask(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop, const char *title,
	const char *question, int yes);

#endif
