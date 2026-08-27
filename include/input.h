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
	INPUT_GREEN,
	INPUT_YELLOW,
	INPUT_BLUE
};

struct input_context {
	int fds[INPUT_MAX_DEVICES];
	char paths[INPUT_MAX_DEVICES][64];
	int count;
};

int input_open(struct input_context *ctx);
void input_close(struct input_context *ctx);
enum input_key input_wait(struct input_context *ctx, int timeout_ms);
const char *input_key_name(enum input_key key);

#endif
