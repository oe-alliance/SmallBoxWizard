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

static process_tick_cb idle_tick;
static unsigned int idle_tick_ms;
static void *idle_opaque;

void process_set_idle(process_tick_cb tick, unsigned int tick_ms, void *opaque)
{
	idle_tick = tick;
	idle_tick_ms = tick_ms;
	idle_opaque = opaque;
}

static void emit_lines(char *pending, size_t *pending_length,
	const char *data, size_t length, process_line_cb callback, void *opaque)
{
	for (size_t i = 0; i < length; ++i) {
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

/* A running command and what its output goes to. */
struct run_state {
	int output_fd;
	pid_t child;
	int status;
	int child_reaped;
	int wait_error;
	char pending[512];
	size_t pending_length;
	process_line_cb callback;
	process_tick_cb tick;
	unsigned int tick_ms;
	uint64_t next_tick;
	uint64_t next_idle_tick;
	void *opaque;
};

static void close_pipe(const int pipe_fds[2])
{
	if (pipe_fds[0] >= 0)
		close(pipe_fds[0]);
	if (pipe_fds[1] >= 0)
		close(pipe_fds[1]);
}

/* In the child: stdout and stderr into output_pipe, stdin from input_pipe or /dev/null. */
__attribute__((noreturn)) static void exec_child(char *const argv[], int with_input, const int output_pipe[2],
	const int input_pipe[2])
{
	dup2(output_pipe[1], STDOUT_FILENO);
	dup2(output_pipe[1], STDERR_FILENO);
	close_pipe(output_pipe);
	if (with_input) {
		dup2(input_pipe[0], STDIN_FILENO);
		close_pipe(input_pipe);
	} else {
		int null_fd = open("/dev/null", O_RDONLY);
		if (null_fd >= 0) {
			dup2(null_fd, STDIN_FILENO);
			close(null_fd);
		}
	}
	execvp(argv[0], argv);  /* NOSONAR the tools of the wizard, argv is built by it */
	dprintf(STDERR_FILENO, "Cannot execute %s: %s\n", argv[0],
		strerror(errno));
	_exit(127);
}

static void write_input(int fd, const char *text)
{
	size_t length = strlen(text);
	size_t written = 0;
	while (written < length) {
		ssize_t result = write(fd, text + written, length - written);  /* NOSONAR the input the wizard gives a tool */
		if (result >= 0)
			written += (size_t)result;
		else if (errno != EINTR)
			break;
	}
}

static void tick_if_due(struct run_state *run)
{
	uint64_t now = current_milliseconds();
	if (run->tick && run->tick_ms && now >= run->next_tick) {
		run->tick(run->opaque);
		run->next_tick = now + run->tick_ms;
	}
	if (idle_tick && idle_tick_ms && now >= run->next_idle_tick) {
		idle_tick(idle_opaque);
		run->next_idle_tick = now + idle_tick_ms;
	}
}

/* How long to wait for output: until the next tick, at most 250 ms, not at all once the child ended. */
static uint64_t wait_time(const struct run_state *run)
{
	uint64_t now;
	uint64_t remaining = 250;
	uint64_t until;
	if (run->child_reaped)
		return 0;
	now = current_milliseconds();
	if (run->tick && run->tick_ms) {
		until = run->next_tick > now ? run->next_tick - now : 0;
		if (until < remaining)
			remaining = until;
	}
	if (idle_tick && idle_tick_ms) {
		until = run->next_idle_tick > now ? run->next_idle_tick - now : 0;
		if (until < remaining)
			remaining = until;
	}
	return remaining;
}

/* One round of reading the output, 0 when the output or the child ended. */
static int run_step(struct run_state *run)
{
	char buffer[256];
	fd_set read_set;
	struct timeval timeout;
	uint64_t remaining;
	ssize_t result;
	int ready;

	if (!run->child_reaped) {
		pid_t waited = waitpid(run->child, &run->status, WNOHANG);
		if (waited == run->child)
			run->child_reaped = 1;
		else if (waited < 0 && errno != EINTR) {
			run->wait_error = 1;
			return 0;
		}
	}
	tick_if_due(run);
	remaining = wait_time(run);
	FD_ZERO(&read_set);
	FD_SET(run->output_fd, &read_set);
	timeout.tv_sec = (time_t)(remaining / 1000ULL);
	timeout.tv_usec = (suseconds_t)((remaining % 1000ULL) * 1000ULL);
	ready = select(run->output_fd + 1, &read_set, NULL, NULL, &timeout);
	if (ready == 0)
		return !run->child_reaped;
	if (ready < 0)
		return errno == EINTR;
	result = read(run->output_fd, buffer, sizeof(buffer));
	if (result > 0) {
		emit_lines(run->pending, &run->pending_length, buffer, (size_t)result, run->callback, run->opaque);
		tick_if_due(run);
		return 1;
	}
	return result < 0 && errno == EINTR;
}

/* The exit code of the child after the output ended, -1 on an error. */
static int run_finish(struct run_state *run)
{
	close(run->output_fd);
	if (run->pending_length) {
		run->pending[run->pending_length] = '\0';
		if (run->callback)
			run->callback(run->pending, run->opaque);
	}
	while (!run->child_reaped && waitpid(run->child, &run->status, 0) < 0)
		if (errno != EINTR)
			return -1;
	if (run->wait_error)
		return -1;
	if (WIFEXITED(run->status))
		return WEXITSTATUS(run->status);
	if (WIFSIGNALED(run->status))
		return 128 + WTERMSIG(run->status);
	return -1;
}

static int process_run_internal(char *const argv[], const char *stdin_text,
	process_line_cb callback, process_tick_cb tick, unsigned int tick_ms,
	void *opaque)
{
	int output_pipe[2] = {-1, -1};
	int input_pipe[2] = {-1, -1};
	struct run_state run = {.callback = callback, .tick = tick, .tick_ms = tick_ms, .opaque = opaque};

	if (!argv || !argv[0]) {
		errno = EINVAL;
		return -1;
	}
	if (pipe(output_pipe) < 0)
		return -1;
	if (stdin_text && pipe(input_pipe) < 0) {
		close_pipe(output_pipe);
		return -1;
	}
	run.child = fork();
	if (run.child < 0) {
		close_pipe(output_pipe);
		close_pipe(input_pipe);
		return -1;
	}
	if (run.child == 0)
		exec_child(argv, stdin_text != NULL, output_pipe, input_pipe);

	close(output_pipe[1]);
	run.output_fd = output_pipe[0];
	if (stdin_text) {
		close(input_pipe[0]);
		write_input(input_pipe[1], stdin_text);
		close(input_pipe[1]);
	}
	{
		uint64_t now = current_milliseconds();
		if (tick && tick_ms)
			run.next_tick = now + tick_ms;
		if (idle_tick && idle_tick_ms)
			run.next_idle_tick = now + idle_tick_ms;
	}
	while (run_step(&run))
		continue;
	return run_finish(&run);
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

	if (!name || !path || path_size == 0)
		return 0;
	if (strchr(name, '/')) {
		if (access(name, X_OK) == 0) {
			snprintf(path, path_size, "%s", name);
			return 1;
		}
		return 0;
	}
	for (int i = 0; directories[i]; ++i) {
		snprintf(path, path_size, "%s/%s", directories[i], name);
		if (access(path, X_OK) == 0)
			return 1;
	}
	path[0] = '\0';
	return 0;
}
