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
	int i;
	if (!ctx)
		return;
	for (i = 0; i < ctx->count; ++i) {
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
		return INPUT_OK;
	case KEY_EXIT:
	case KEY_ESC:
	case KEY_BACK:
	case KEY_BACKSPACE:
		return INPUT_BACK;
	case KEY_RED: return INPUT_RED;
	case KEY_GREEN: return INPUT_GREEN;
	case KEY_YELLOW: return INPUT_YELLOW;
	case KEY_BLUE: return INPUT_BLUE;
	default: return INPUT_NONE;
	}
}

enum input_key input_wait(struct input_context *ctx, int timeout_ms)
{
	fd_set read_set;
	struct timeval timeout;
	int maximum = -1;
	int i;
	int result;

	if (!ctx || ctx->count == 0) {
		if (timeout_ms > 0)
			usleep((useconds_t)timeout_ms * 1000U);
		return INPUT_NONE;
	}
	FD_ZERO(&read_set);
	for (i = 0; i < ctx->count; ++i) {
		if (ctx->fds[i] >= 0) {
			FD_SET(ctx->fds[i], &read_set);
			if (ctx->fds[i] > maximum)
				maximum = ctx->fds[i];
		}
	}
	timeout.tv_sec = timeout_ms / 1000;
	timeout.tv_usec = (timeout_ms % 1000) * 1000;
	result = select(maximum + 1, &read_set, NULL, NULL,
		timeout_ms < 0 ? NULL : &timeout);
	if (result <= 0)
		return INPUT_NONE;
	for (i = 0; i < ctx->count; ++i) {
		struct input_event event;
		ssize_t length;
		if (ctx->fds[i] < 0 || !FD_ISSET(ctx->fds[i], &read_set))
			continue;
		while ((length = read(ctx->fds[i], &event, sizeof(event))) ==
			(ssize_t)sizeof(event)) {
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
	}
	return INPUT_NONE;
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
	case INPUT_GREEN: return "GREEN";
	case INPUT_YELLOW: return "YELLOW";
	case INPUT_BLUE: return "BLUE";
	default: return "NONE";
	}
}
