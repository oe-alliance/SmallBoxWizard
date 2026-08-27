#define _GNU_SOURCE

#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static uint64_t current_milliseconds(void)
{
	struct timeval now;
	if (gettimeofday(&now, NULL) != 0)
		return 0;
	return (uint64_t)now.tv_sec * 1000ULL +
		(uint64_t)now.tv_usec / 1000ULL;
}

static void emit_lines(char *pending, size_t *pending_length,
	const char *data, size_t length, process_line_cb callback, void *opaque)
{
	size_t i;

	for (i = 0; i < length; ++i) {
		char ch = data[i];
		if (ch == '\r')
			continue;
		if (ch == '\n' || *pending_length + 1 >= 512) {
			pending[*pending_length] = '\0';
			if (callback && *pending_length)
				callback(pending, opaque);
			*pending_length = 0;
			continue;
		}
		pending[(*pending_length)++] = ch;
	}
}

static int process_run_internal(char *const argv[], const char *stdin_text,
	process_line_cb callback, process_tick_cb tick, unsigned int tick_ms,
	void *opaque)
{
	int output_pipe[2] = {-1, -1};
	int input_pipe[2] = {-1, -1};
	pid_t child;
	int status = 0;
	char buffer[256];
	char pending[512];
	size_t pending_length = 0;
	uint64_t next_tick = 0;
	int child_reaped = 0;
	int wait_error = 0;

	if (!argv || !argv[0]) {
		errno = EINVAL;
		return -1;
	}
	if (pipe(output_pipe) < 0)
		return -1;
	if (stdin_text && pipe(input_pipe) < 0) {
		close(output_pipe[0]);
		close(output_pipe[1]);
		return -1;
	}

	child = fork();
	if (child < 0) {
		close(output_pipe[0]);
		close(output_pipe[1]);
		if (input_pipe[0] >= 0) {
			close(input_pipe[0]);
			close(input_pipe[1]);
		}
		return -1;
	}
	if (child == 0) {
		int null_fd;
		dup2(output_pipe[1], STDOUT_FILENO);
		dup2(output_pipe[1], STDERR_FILENO);
		close(output_pipe[0]);
		close(output_pipe[1]);
		if (stdin_text) {
			dup2(input_pipe[0], STDIN_FILENO);
			close(input_pipe[0]);
			close(input_pipe[1]);
		} else {
			null_fd = open("/dev/null", O_RDONLY);
			if (null_fd >= 0) {
				dup2(null_fd, STDIN_FILENO);
				close(null_fd);
			}
		}
		execvp(argv[0], argv);
		dprintf(STDERR_FILENO, "Cannot execute %s: %s\n", argv[0],
			strerror(errno));
		_exit(127);
	}

	close(output_pipe[1]);
	if (stdin_text) {
		size_t length = strlen(stdin_text);
		size_t written = 0;
		close(input_pipe[0]);
		while (written < length) {
			ssize_t result = write(input_pipe[1], stdin_text + written,
				length - written);
			if (result < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			written += (size_t)result;
		}
		close(input_pipe[1]);
	}
	if (tick && tick_ms)
		next_tick = current_milliseconds() + tick_ms;

	for (;;) {
		ssize_t result;
		fd_set read_set;
		struct timeval timeout;
		uint64_t now;
		uint64_t remaining = 250;
		int ready;

		if (!child_reaped) {
			pid_t waited = waitpid(child, &status, WNOHANG);
			if (waited == child)
				child_reaped = 1;
			else if (waited < 0 && errno != EINTR) {
				wait_error = 1;
				break;
			}
		}
		now = current_milliseconds();
		if (tick && tick_ms) {
			if (now >= next_tick) {
				tick(opaque);
				next_tick = now + tick_ms;
			}
			remaining = next_tick > now ? next_tick - now : 0;
			if (remaining > 250)
				remaining = 250;
		}
		if (child_reaped)
			remaining = 0;
		FD_ZERO(&read_set);
		FD_SET(output_pipe[0], &read_set);
		timeout.tv_sec = (time_t)(remaining / 1000ULL);
		timeout.tv_usec = (suseconds_t)((remaining % 1000ULL) * 1000ULL);
		ready = select(output_pipe[0] + 1, &read_set, NULL, NULL, &timeout);
		if (ready == 0) {
			if (child_reaped)
				break;
			continue;
		}
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		result = read(output_pipe[0], buffer, sizeof(buffer));
		if (result > 0) {
			emit_lines(pending, &pending_length, buffer, (size_t)result,
				callback, opaque);
			if (tick && tick_ms) {
				uint64_t now = current_milliseconds();
				if (now >= next_tick) {
					tick(opaque);
					next_tick = now + tick_ms;
				}
			}
			continue;
		}
		if (result < 0 && errno == EINTR)
			continue;
		break;
	}
	close(output_pipe[0]);
	if (pending_length) {
		pending[pending_length] = '\0';
		if (callback)
			callback(pending, opaque);
	}
	if (!child_reaped) {
		while (waitpid(child, &status, 0) < 0) {
			if (errno != EINTR)
				return -1;
		}
	}
	if (wait_error)
		return -1;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return -1;
}

int process_run(char *const argv[], const char *stdin_text,
	process_line_cb callback, void *opaque)
{
	return process_run_internal(argv, stdin_text, callback, NULL, 0, opaque);
}

int process_run_with_updates(char *const argv[], const char *stdin_text,
	process_line_cb callback, process_tick_cb tick, unsigned int tick_ms,
	void *opaque)
{
	return process_run_internal(argv, stdin_text, callback, tick, tick_ms,
		opaque);
}

struct capture_state {
	char *output;
	size_t output_size;
	size_t used;
};

static void capture_line(const char *line, void *opaque)
{
	struct capture_state *state = opaque;
	size_t available;
	size_t length;

	if (!state || state->used + 1 >= state->output_size)
		return;
	available = state->output_size - state->used - 1;
	length = strlen(line);
	if (length > available)
		length = available;
	memcpy(state->output + state->used, line, length);
	state->used += length;
	state->output[state->used] = '\0';
}

int process_capture(char *const argv[], char *output, size_t output_size)
{
	struct capture_state state;

	if (!output || output_size == 0) {
		errno = EINVAL;
		return -1;
	}
	output[0] = '\0';
	state.output = output;
	state.output_size = output_size;
	state.used = 0;
	return process_run(argv, NULL, capture_line, &state);
}

int process_find(const char *name, char *path, size_t path_size)
{
	static const char *const directories[] = {
		"/usr/sbin", "/usr/bin", "/sbin", "/bin", NULL
	};
	int i;

	if (!name || !path || path_size == 0)
		return 0;
	if (strchr(name, '/')) {
		if (access(name, X_OK) == 0) {
			snprintf(path, path_size, "%s", name);
			return 1;
		}
		return 0;
	}
	for (i = 0; directories[i]; ++i) {
		snprintf(path, path_size, "%s/%s", directories[i], name);
		if (access(path, X_OK) == 0)
			return 1;
	}
	path[0] = '\0';
	return 0;
}
