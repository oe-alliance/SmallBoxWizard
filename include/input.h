#ifndef SMALLBOX_INPUT_H
#define SMALLBOX_INPUT_H

#include <stddef.h>

#define INPUT_MAX_DEVICES 32

enum input_key {
	INPUT_NONE = 0,
	INPUT_UP,
	INPUT_DOWN,
	INPUT_LEFT,
	INPUT_RIGHT,
	INPUT_OK,
	INPUT_BACK,
	INPUT_RED,
	INPUT_YELLOW,
	INPUT_BLUE,
	INPUT_HELP,
	INPUT_INFO,
	INPUT_0,  /* INPUT_0 + n is the digit n. */
	INPUT_1,
	INPUT_2,
	INPUT_3,
	INPUT_4,
	INPUT_5,
	INPUT_6,
	INPUT_7,
	INPUT_8,
	INPUT_9
};

struct input_context {
	int fds[INPUT_MAX_DEVICES];
	char paths[INPUT_MAX_DEVICES][64];
	int count;
};

int input_open(struct input_context *ctx);
void input_close(struct input_context *ctx);
enum input_key input_wait(const struct input_context *ctx, int timeout_ms);
/* Like input_wait, opens the devices again when none is left, e.g. after an unplugged keyboard. */
enum input_key input_next(struct input_context *ctx, int timeout_ms);
/* Runs before and after every input_next, e.g. for work in the background; NULL removes it. */
void input_set_idle(void (*idle)(void));
/* Gets every key first, e.g. INFO for About over every screen; a key it handled, which it
 * returns 1 for, comes back as INPUT_NONE. */
void input_set_global(int (*global)(enum input_key key));
/* While pending returns 1, input_next returns BACK at once. */
void input_set_jump(int (*pending)(void));
/* 1 while the BACK of input_next only leads to another entry of the menu. */
int input_jumping(void);
/* Runs after every input_next, e.g. to update the clock of the screen. */
void input_set_clock(void (*clock)(void));
/* Gets every key input_next reads, before the global keys, e.g. to show it in the footer. */
void input_set_press(void (*pressed)(enum input_key key));
const char *input_key_name(enum input_key key);

#endif
