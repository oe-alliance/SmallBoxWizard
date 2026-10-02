#define _GNU_SOURCE

#include "input.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#define BITS_PER_LONG (sizeof(unsigned long) * 8U)
#define BITS_TO_LONGS(bits) (((bits) + BITS_PER_LONG - 1U) / BITS_PER_LONG)
#define TEST_BIT(bit, array) (((array)[(bit) / BITS_PER_LONG] >> \
	((bit) % BITS_PER_LONG)) & 1UL)

static int compare_names(const struct dirent **left, const struct dirent **right)
{
	return strcmp((*left)->d_name, (*right)->d_name);
}

static int input_debug_enabled(void)
{
	static int initialized;
	static int enabled;
	if (!initialized) {
		enabled = getenv("SMALLBOX_INPUT_DEBUG") != NULL;
		initialized = 1;
	}
	return enabled;
}

static int is_navigation_device(int fd)
{
	unsigned long events[BITS_TO_LONGS(EV_MAX + 1)];
	unsigned long keys[BITS_TO_LONGS(KEY_MAX + 1)];

	memset(events, 0, sizeof(events));
	memset(keys, 0, sizeof(keys));
	if (ioctl(fd, EVIOCGBIT(0, sizeof(events)), events) < 0 ||
		!TEST_BIT(EV_KEY, events))
		return 0;
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) < 0)
		return 0;
	return (TEST_BIT(KEY_UP, keys) && TEST_BIT(KEY_DOWN, keys)) ||
		TEST_BIT(KEY_OK, keys) || TEST_BIT(KEY_ENTER, keys);
}

int input_open(struct input_context *ctx)
{
	struct dirent **entries = NULL;
	int count;
	int i;

	if (!ctx)
		return 0;
	memset(ctx, 0, sizeof(*ctx));
	for (i = 0; i < INPUT_MAX_DEVICES; ++i)
		ctx->fds[i] = -1;
	count = scandir("/dev/input", &entries, NULL, compare_names);
	if (count < 0)
		return 0;
	for (i = 0; i < count && ctx->count < INPUT_MAX_DEVICES; ++i) {
		int fd;
		char path[64];
		if (strncmp(entries[i]->d_name, "event", 5) != 0) {
			free(entries[i]);
			continue;
		}
		snprintf(path, sizeof(path), "/dev/input/%.52s", entries[i]->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd >= 0 && is_navigation_device(fd)) {
			ctx->fds[ctx->count] = fd;
			snprintf(ctx->paths[ctx->count],
				sizeof(ctx->paths[ctx->count]), "%s", path);
			if (input_debug_enabled()) {
				fprintf(stderr, "Input device: %s\n", path);
				fflush(stderr);
			}
			ctx->count++;
		} else if (fd >= 0) {
			close(fd);
		}
		free(entries[i]);
	}
	free(entries);
	return ctx->count;
}

void input_close(struct input_context *ctx)
{
	if (!ctx)
		return;
	for (int i = 0; i < ctx->count; ++i) {
		if (ctx->fds[i] >= 0)
			close(ctx->fds[i]);
		ctx->fds[i] = -1;
	}
	ctx->count = 0;
}

static enum input_key map_key(unsigned int code)
{
	switch (code) {
	case KEY_UP: return INPUT_UP;
	case KEY_DOWN: return INPUT_DOWN;
	case KEY_LEFT: return INPUT_LEFT;
	case KEY_RIGHT: return INPUT_RIGHT;
	case KEY_OK:
	case KEY_ENTER:
	case KEY_SELECT:
	case KEY_GREEN:  /* Confirms like OK in every screen. */
		return INPUT_OK;
	case KEY_EXIT:
	case KEY_ESC:
	case KEY_BACK:
	case KEY_BACKSPACE:
		return INPUT_BACK;
	case KEY_RED: return INPUT_RED;
	case KEY_YELLOW: return INPUT_YELLOW;
	case KEY_BLUE: return INPUT_BLUE;
	case KEY_HELP: return INPUT_HELP;
	case KEY_INFO: return INPUT_INFO;
	case KEY_0: return INPUT_0;
	case KEY_1: case KEY_2: case KEY_3: case KEY_4: case KEY_5:
	case KEY_6: case KEY_7: case KEY_8: case KEY_9:
		return (enum input_key)(INPUT_1 + (code - KEY_1));
	default: return INPUT_NONE;
	}
}

/* The highest descriptor in read_set, -1 without any. */
static int fill_set(const struct input_context *ctx, fd_set *read_set)
{
	int maximum = -1;
	FD_ZERO(read_set);
	for (int i = 0; i < ctx->count; ++i) {
		if (ctx->fds[i] >= 0) {
			FD_SET(ctx->fds[i], read_set);
			if (ctx->fds[i] > maximum)
				maximum = ctx->fds[i];
		}
	}
	return maximum;
}

/* The first key pressed among the pending events of device i. */
static enum input_key read_device(const struct input_context *ctx, int i)
{
	struct input_event event;
	while (read(ctx->fds[i], &event, sizeof(event)) == (ssize_t)sizeof(event)) {
		enum input_key key;
		if (input_debug_enabled()) {
			fprintf(stderr, "Input event: device=%s type=%u code=%u value=%d\n",
				ctx->paths[i], event.type, event.code, event.value);
			fflush(stderr);
		}
		if (event.type != EV_KEY || event.value == 0)
			continue;
		key = map_key(event.code);
		if (key != INPUT_NONE)
			return key;
	}
	return INPUT_NONE;
}

enum input_key input_wait(const struct input_context *ctx, int timeout_ms)
{
	fd_set read_set;
	struct timeval timeout;
	int maximum;
	int result;

	if (!ctx || ctx->count == 0) {
		if (timeout_ms > 0) {
			struct timespec delay = {timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
			nanosleep(&delay, NULL);
		}
		return INPUT_NONE;
	}
	maximum = fill_set(ctx, &read_set);
	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_usec = (timeout_ms % 1000) * 1000;
	result = select(maximum + 1, &read_set, NULL, NULL,
		timeout_ms < 0 ? NULL : &timeout);
	if (result <= 0)
		return INPUT_NONE;
	for (int i = 0; i < ctx->count; ++i) {
		enum input_key key;
		if (ctx->fds[i] < 0 || !FD_ISSET(ctx->fds[i], &read_set))
			continue;
		key = read_device(ctx, i);
		if (key != INPUT_NONE)
			return key;
	}
	return INPUT_NONE;
}

static void (*idle_task)(void);

void input_set_idle(void (*idle)(void))
{
	idle_task = idle;
}

static int (*global_task)(enum input_key key);

void input_set_global(int (*global)(enum input_key key))
{
	global_task = global;
}

static int (*jump_task)(void);

void input_set_jump(int (*pending)(void))
{
	jump_task = pending;
}

int input_jumping(void)
{
	return jump_task && jump_task();
}

static void (*clock_task)(void);

void input_set_clock(void (*clock)(void))
{
	clock_task = clock;
}

static void (*press_task)(enum input_key key);

void input_set_press(void (*pressed)(enum input_key key))
{
	press_task = pressed;
}

enum input_key input_next(struct input_context *ctx, int timeout_ms)
{
	enum input_key key;
	if (jump_task && jump_task())  /* Every screen goes back until the menu opens the other entry. */
		return INPUT_BACK;
	if (idle_task)
		idle_task();
	key = input_wait(ctx, idle_task && timeout_ms > 300 ? 300 : timeout_ms);  /* Short, so the task comes soon. */
	if (ctx->count == 0) {
		input_close(ctx);
		input_open(ctx);
	}
	if (key != INPUT_NONE && press_task)
		press_task(key);
	if (key != INPUT_NONE && global_task && global_task(key))
		key = INPUT_NONE;
	if (idle_task)
		idle_task();
	if (clock_task)
		clock_task();
	return key;
}

const char *input_key_name(enum input_key key)
{
	switch (key) {
	case INPUT_UP: return "UP";
	case INPUT_DOWN: return "DOWN";
	case INPUT_LEFT: return "LEFT";
	case INPUT_RIGHT: return "RIGHT";
	case INPUT_OK: return "OK";
	case INPUT_BACK: return "BACK";
	case INPUT_RED: return "RED";
	case INPUT_YELLOW: return "YELLOW";
	case INPUT_BLUE: return "BLUE";
	case INPUT_HELP: return "HELP";
	case INPUT_INFO: return "INFO";
	case INPUT_0: return "0";
	case INPUT_1: return "1";
	case INPUT_2: return "2";
	case INPUT_3: return "3";
	case INPUT_4: return "4";
	case INPUT_5: return "5";
	case INPUT_6: return "6";
	case INPUT_7: return "7";
	case INPUT_8: return "8";
	case INPUT_9: return "9";
	default: return "NONE";
	}
}
