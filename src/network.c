#define _GNU_SOURCE

#include "network.h"
#include "process.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define OPKG_WORK_ROOT "/.FlashExpander/.smallbox-opkg"
#define OPKG_CACHE_DIR OPKG_WORK_ROOT "/cache"
#define OPKG_TMP_DIR OPKG_WORK_ROOT "/tmp"
#define OPKG_LISTS_DIR OPKG_WORK_ROOT "/lists"
#define OPKG_LOG_FILE OPKG_WORK_ROOT "/install.log"
#define OPKG_SKIPPED_FILE "/etc/smallbox-wizard.skipped-packages"
#define MAX_RECOVERY_SKIPS 512
#define MINIMUM_TLS_TIME ((time_t)1577836800)

__attribute__((format(printf, 3, 4)))
static void set_error(char *error, size_t error_size, const char *format, ...)  /* NOSONAR printf-style error text */
{
	va_list arguments;
	if (!error || error_size == 0)
		return;
	va_start(arguments, format);
	vsnprintf(error, error_size, format, arguments);  /* NOSONAR format is checked by the printf attribute */
	va_end(arguments);
}

static int safe_interface_name(const char *name)
{
	const unsigned char *cursor = (const unsigned char *)name;
	if (!name || !name[0] || strlen(name) >= IFNAMSIZ)
		return 0;
	while (*cursor) {
		if (!((*cursor >= 'a' && *cursor <= 'z') ||
			(*cursor >= 'A' && *cursor <= 'Z') ||
			(*cursor >= '0' && *cursor <= '9') ||
			*cursor == '_' || *cursor == '-' || *cursor == '.'))
			return 0;
		cursor++;
	}
	return 1;
}

static int ensure_directory(const char *path, mode_t mode)
{
	struct stat status;
	if (mkdir(path, mode) == 0)
		return 1;
	return errno == EEXIST && stat(path, &status) == 0 &&
		S_ISDIR(status.st_mode);
}

static int get_ipv4(const char *interface, char *address, size_t address_size)
{
	int socket_fd;
	struct ifreq request;
	const struct sockaddr_in *internet;
	socklen_t text_size;
	if (!address || address_size == 0 || !safe_interface_name(interface))
		return 0;
	/* inet_ntop needs at most INET_ADDRSTRLEN bytes */
	text_size = address_size > INET_ADDRSTRLEN ? INET_ADDRSTRLEN : (socklen_t)address_size;
	socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (socket_fd < 0)
		return 0;
	memset(&request, 0, sizeof(request));
	snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", interface);
	if (ioctl(socket_fd, SIOCGIFADDR, &request) < 0) {
		close(socket_fd);
		return 0;
	}
	close(socket_fd);
	internet = (const struct sockaddr_in *)&request.ifr_addr;
	if (!inet_ntop(AF_INET, &internet->sin_addr, address, text_size))
		return 0;
	return strcmp(address, "0.0.0.0") != 0;
}

static int read_int(const char *path, int fallback)
{
	FILE *file = fopen(path, "r");
	int value = fallback;
	if (!file)
		return fallback;
	if (fscanf(file, "%d", &value) != 1)
		value = fallback;
	fclose(file);
	return value;
}

static int interface_compare(const void *left, const void *right)
{
	const struct network_interface *a = left;
	const struct network_interface *b = right;
	if (a->wireless != b->wireless)
		return a->wireless - b->wireless;
	return strcmp(a->name, b->name);
}

int network_scan(struct network_interface interfaces[], int maximum)
{
	DIR *directory;
	const struct dirent *entry;
	int count = 0;
	if (!interfaces || maximum <= 0)
		return 0;
	directory = opendir("/sys/class/net");
	if (!directory)
		return 0;
	while ((entry = readdir(directory)) != NULL && count < maximum) {
		struct network_interface *interface;
		char path[256];
		size_t name_length;
		if (entry->d_name[0] == '.' || strcmp(entry->d_name, "lo") == 0 ||
			!safe_interface_name(entry->d_name) ||
			if_nametoindex(entry->d_name) == 0)
			continue;
		interface = &interfaces[count];
		memset(interface, 0, sizeof(*interface));
		name_length = strlen(entry->d_name);
		memcpy(interface->name, entry->d_name, name_length + 1);
		snprintf(path, sizeof(path), "/sys/class/net/%s/wireless",
			interface->name);
		interface->wireless = access(path, F_OK) == 0 ||
			strncmp(interface->name, "wlan", 4) == 0 ||
			strncmp(interface->name, "ra", 2) == 0;
		snprintf(path, sizeof(path), "/sys/class/net/%s/carrier",
			interface->name);
		interface->link = read_int(path, 0) == 1;
		get_ipv4(interface->name, interface->address,
			sizeof(interface->address));
		count++;
	}
	closedir(directory);
	qsort(interfaces, (size_t)count, sizeof(interfaces[0]), interface_compare);
	return count;
}

int network_has_ipv4(char *interface, size_t interface_size,
	char *address, size_t address_size)
{
	struct network_interface interfaces[NETWORK_MAX_INTERFACES];
	int count = network_scan(interfaces, NETWORK_MAX_INTERFACES);
	for (int i = 0; i < count; ++i) {
		if (interfaces[i].address[0]) {
			if (interface && interface_size)
				snprintf(interface, interface_size, "%s", interfaces[i].name);
			if (address && address_size)
				snprintf(address, address_size, "%s", interfaces[i].address);
			return 1;
		}
	}
	return 0;
}

int network_synchronize_time(network_progress_cb callback, void *opaque,
	char *error, size_t error_size)
{
	char chronyc[PATH_MAX];

	if (time(NULL) >= MINIMUM_TLS_TIME)
		return 1;
	if (!process_find("chronyc", chronyc, sizeof(chronyc))) {
		set_error(error, error_size,
			"Chrony is not installed. A valid system time is required for secure downloads.");
		return 0;
	}
	if (callback)
		callback(1, "Synchronizing the system time with Chrony...", opaque);
	if (access("/etc/init.d/chronyd", X_OK) == 0) {
		char *start_argv[] = {"/etc/init.d/chronyd", "start", NULL};
		process_run(start_argv, NULL, NULL, NULL);
	}
	{
		char *online_argv[] = {chronyc, "-a", "online", NULL};
		char *step_argv[] = {chronyc, "-a", "makestep", NULL};
		process_run(online_argv, NULL, NULL, NULL);
		process_run(step_argv, NULL, NULL, NULL);
	}
	for (int second = 0; second < 45; ++second) {
		char status[128];
		if (time(NULL) >= MINIMUM_TLS_TIME) {
			if (callback)
				callback(4, "The system time has been synchronized.", opaque);
			return 1;
		}
		snprintf(status, sizeof(status),
			"Waiting for Chrony time synchronization... %d / 45 s",
			second + 1);
		if (callback)
			callback(1 + second * 2 / 45, status, opaque);
		sleep(1);
		if ((second + 1) % 5 == 0) {
			char *step_argv[] = {chronyc, "-a", "makestep", NULL};
			process_run(step_argv, NULL, NULL, NULL);
		}
	}
	set_error(error, error_size,
		"Chrony could not synchronize the system time. Check the Internet connection and try again.");
	return 0;
}

static int write_all(int fd, const char *data, size_t length)
{
	size_t used = 0;
	while (used < length) {
		ssize_t result = write(fd, data + used, length - used);  /* NOSONAR fd is the wizard's config backup */
		if (result < 0)
			return 0;
		used += (size_t)result;
	}
	return 1;
}

static int backup_file_once(const char *source, const char *backup)
{
	char buffer[4096];
	int source_fd;
	int backup_fd;
	ssize_t length;
	source_fd = open(source, O_RDONLY | O_CLOEXEC);
	if (source_fd < 0) {
		/* nothing to back up, or a backup exists already */
		if (errno == ENOENT)
			return 1;
		return access(backup, F_OK) == 0;
	}
	backup_fd = open(backup, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
	if (backup_fd < 0) {
		close(source_fd);
		return errno == EEXIST;
	}
	while ((length = read(source_fd, buffer, sizeof(buffer))) > 0) {
		if (!write_all(backup_fd, buffer, (size_t)length)) {
			close(source_fd);
			close(backup_fd);
			unlink(backup);
			return 0;
		}
	}
	fsync(backup_fd);
	close(source_fd);
	close(backup_fd);
	return length == 0;
}

static int write_interfaces_file(const char *selected, char *error,
	size_t error_size)
{
	struct network_interface interfaces[NETWORK_MAX_INTERFACES];
	char temporary[] = "/etc/network/interfaces.smallbox.XXXXXX";
	int count = network_scan(interfaces, NETWORK_MAX_INTERFACES);
	int fd;
	FILE *file;
	int found = 0;
	int failed = 0;
	int saved_errno = 0;

	for (int i = 0; i < count; ++i) {
		if (strcmp(interfaces[i].name, selected) == 0 &&
			!interfaces[i].wireless)
			found = 1;
	}
	if (!found) {
		set_error(error, error_size, "The LAN interface is no longer available.");
		return 0;
	}
	if (!backup_file_once("/etc/network/interfaces",
		"/etc/network/interfaces.smallbox-wizard.bak")) {
		set_error(error, error_size, "The network configuration cannot be backed up.");
		return 0;
	}
	fd = mkstemp(temporary);
	if (fd < 0) {
		set_error(error, error_size, "The network configuration cannot be created: %s",
			strerror(errno));
		return 0;
	}
	fchmod(fd, 0644);
	file = fdopen(fd, "w");
	if (!file) {
		close(fd);
		unlink(temporary);
		set_error(error, error_size, "The network configuration cannot be written.");
		return 0;
	}
	fputs("# Automatically generated by SmallBox Wizard.\n\n", file);
	fputs("auto lo\niface lo inet loopback\n\n", file);
	for (int i = 0; i < count; ++i) {
		const char *prefix = strcmp(interfaces[i].name, selected) == 0 ? "" : "# ";
		fprintf(file, "%sauto %s\n", prefix, interfaces[i].name);
		fprintf(file, "# iface %s inet6 dhcp\n", interfaces[i].name);
		fprintf(file, "%siface %s inet dhcp\n\n", prefix,
			interfaces[i].name);
	}
	if (fflush(file) != 0 || fsync(fd) != 0) {
		failed = 1;
		saved_errno = errno;
	}
	if (fclose(file) != 0 && !failed) {
		failed = 1;
		saved_errno = errno;
	}
	if (!failed && rename(temporary, "/etc/network/interfaces") != 0) {
		failed = 1;
		saved_errno = errno;
	}
	if (failed) {
		set_error(error, error_size, "The network configuration could not be saved: %s",
			strerror(saved_errno));
		unlink(temporary);
		return 0;
	}
	return 1;
}

static void progress(network_progress_cb callback, void *opaque, int percent,
	const char *status)
{
	if (callback)
		callback(percent, status, opaque);
}

int network_configure_dhcp(const char *interface,
	network_progress_cb callback, void *opaque,
	char *address, size_t address_size, char *error, size_t error_size)
{
	char ifup[256];
	char ifdown[256];
	char name[IFNAMSIZ];
	if (address && address_size) address[0] = '\0';
	if (error && error_size) error[0] = '\0';
	if (!safe_interface_name(interface)) {
		set_error(error, error_size, "Invalid network interface name.");
		return 0;
	}
	snprintf(name, sizeof(name), "%s", interface);
	progress(callback, opaque, 10, "Saving the DHCP configuration...");
	if (!write_interfaces_file(interface, error, error_size))
		return 0;
	if (!process_find("ifup", ifup, sizeof(ifup))) {
		set_error(error, error_size, "ifup is not installed in this image.");
		return 0;
	}
	if (process_find("ifdown", ifdown, sizeof(ifdown))) {
		char *down_argv[] = {ifdown, name, NULL};
		process_run(down_argv, NULL, NULL, NULL);
	}
	progress(callback, opaque, 35, "Activating LAN and waiting for DHCP...");
	{
		char *up_argv[] = {ifup, name, NULL};
		int result = process_run(up_argv, NULL, NULL, NULL);
		if (result != 0 && !get_ipv4(interface, address, address_size)) {
			set_error(error, error_size, "ifup/DHCP failed with status %d.",
				result);
			return 0;
		}
	}
	for (int attempt = 0; attempt < 20; ++attempt) {
		if (get_ipv4(interface, address, address_size)) {
			progress(callback, opaque, 100, "An IP address has been assigned.");
			return 1;
		}
		progress(callback, opaque, 50 + attempt * 2,
			"Waiting for an IP address from the router...");
		sleep(1);
	}
	set_error(error, error_size, "No IPv4 address was received via DHCP.");
	return 0;
}

struct package_entry {
	char name[NETWORK_PACKAGE_NAME_SIZE];
	int installed;
	int skipped;
	int seen_action;
};

struct package_plan {
	struct package_entry *entries;
	int count;
	int capacity;
	FILE *log;
};

struct install_state {
	network_progress_cb callback;
	void *opaque;
	struct package_plan *plan;
	FILE *log;
	int percent;
	int phase;
	int planned;
	int completed;
	int verified_installed;
	int skipped;
	time_t started;
	char line[256];
	char current_package[NETWORK_PACKAGE_NAME_SIZE];
	char failure_package[NETWORK_PACKAGE_NAME_SIZE];
	char failure_detail[NETWORK_PACKAGE_DETAIL_SIZE];
	int failure_score;
};

enum {
	INSTALL_PHASE_UPDATE,
	INSTALL_PHASE_PACKAGES
};

static int copy_package_token(const char *text, char *package,
	size_t package_size)
{
	size_t length = 0;

	if (!text || !package || package_size == 0)
		return 0;
	while (*text == ' ' || *text == '\t' || *text == '\'' || *text == '"')
		text++;
	if (!((text[0] >= 'a' && text[0] <= 'z') ||
		(text[0] >= 'A' && text[0] <= 'Z') ||
		(text[0] >= '0' && text[0] <= '9')))
		return 0;
	while ((text[length] >= 'a' && text[length] <= 'z') ||
		(text[length] >= 'A' && text[length] <= 'Z') ||
		(text[length] >= '0' && text[length] <= '9') ||
		text[length] == '-' || text[length] == '+' ||
		text[length] == '.' || text[length] == '_')
		length++;
	while (length && (text[length - 1] == '.' || text[length - 1] == ','))
		length--;
	if (!length || length >= package_size)
		return 0;
	memcpy(package, text, length);
	package[length] = '\0';
	return 1;
}

static int hexadecimal_hash(const char *text, size_t length)
{
	if (!text || (length != 32 && length != 40 && length != 64))
		return 0;
	for (size_t i = 0; i < length; ++i) {
		if (!((text[i] >= '0' && text[i] <= '9') ||
			(text[i] >= 'a' && text[i] <= 'f') ||
			(text[i] >= 'A' && text[i] <= 'F')))
			return 0;
	}
	return 1;
}

/* Index of the first '_' in text[0..length), or length. */
static size_t underscore_index(const char *text, size_t length)
{
	size_t index = 0;
	while (index < length && text[index] != '_')
		index++;
	return index;
}

static int package_from_ipk_path(const char *line, char *package,
	size_t package_size)
{
	const char *end;
	size_t start;
	size_t stop;
	size_t separator;
	size_t length;

	if (!line || !package || package_size == 0)
		return 0;
	end = strstr(line, ".ipk");
	if (!end)
		return 0;
	stop = (size_t)(end - line);
	start = stop;
	while (start > 0 && line[start - 1] != '/' && line[start - 1] != ' ' &&
		line[start - 1] != '\t' && line[start - 1] != '\'' && line[start - 1] != '"')
		start--;
	/* start <= separator <= stop */
	separator = start + underscore_index(line + start, stop - start);
	if (separator < stop && hexadecimal_hash(line + start, separator - start)) {
		start = separator + 1;
		separator = start + underscore_index(line + start, stop - start);
	}
	length = separator - start;
	if (!length || length >= package_size)
		return 0;
	memcpy(package, line + start, length);
	package[length] = '\0';
	return 1;
}

static int action_package(const char *line, char *package, size_t package_size)
{
	static const struct {
		const char *prefix;
		size_t length;
	} actions[] = {
		{"Installing ", 11},
		{"Upgrading ", 10},
		{"Configuring ", 12}
	};

	if (!line)
		return 0;
	for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
		if (strncmp(line, actions[i].prefix, actions[i].length) == 0)
			return copy_package_token(line + actions[i].length, package,
				package_size);
	}
	if (strncmp(line, "Downloading ", 12) == 0)
		return package_from_ipk_path(line + 12, package, package_size);
	return 0;
}

static int package_action_line(const char *line)
{
	return line && (strncmp(line, "Installing ", 11) == 0 ||
		strncmp(line, "Upgrading ", 10) == 0);
}

static int error_line(const char *line)
{
	return line && (strstr(line, "ERROR") || strstr(line, "Error") ||
		strstr(line, "error") || strstr(line, "Failed") ||
		strstr(line, "failed") || strstr(line, "Cannot") ||
		strstr(line, "cannot") || strstr(line, "mismatch") ||
		strstr(line, "returned status") || strstr(line, "Collected errors"));
}

static int error_package(const char *line, char *package, size_t package_size)
{
	static const char *const prefixes[] = {
		"Failed to download ",
		"failed to download ",
		"Failed to install ",
		"failed to install ",
		"Failed to extract ",
		"failed to extract ",
		"Could not install ",
		"could not install ",
		"Cannot install package ",
		"cannot install package ",
		"No package ",
		"Package ",
		"package ",
		"Packages for ",
		NULL
	};
	const char *quoted;

	if (!line)
		return 0;
	if (package_from_ipk_path(line, package, package_size))
		return 1;
	quoted = strstr(line, "package \"");
	if (quoted && copy_package_token(quoted + 9, package, package_size))
		return 1;
	quoted = strstr(line, "Package \"");
	if (quoted && copy_package_token(quoted + 9, package, package_size))
		return 1;
	for (int i = 0; prefixes[i]; ++i) {
		const char *match = strstr(line, prefixes[i]);
		if (match && copy_package_token(match + strlen(prefixes[i]), package,
			package_size))
			return 1;
	}
	return 0;
}

static int plan_find(const struct package_plan *plan, const char *package)
{
	if (!plan || !package || !package[0])
		return -1;
	for (int i = 0; i < plan->count; ++i) {
		if (strcmp(plan->entries[i].name, package) == 0)
			return i;
	}
	return -1;
}

static int failure_package_score(const struct package_plan *plan,
	const char *package)
{
	if (!package || !package[0])
		return 0;
	if (hexadecimal_hash(package, strlen(package)))
		return 0;
	if (strncmp(package, "packagegroup-", 13) == 0)
		return 1;
	if (plan_find(plan, package) >= 0)
		return 4;
	return 3;
}

static int plan_add(struct package_plan *plan, const char *package)
{
	struct package_entry *resized;
	struct package_entry *entry;
	int capacity;

	if (!plan || !package || !package[0] ||
		strncmp(package, "packagegroup-", 13) == 0)
		return 1;
	if (plan_find(plan, package) >= 0)
		return 1;
	if (plan->count < 0 || plan->count > plan->capacity)
		return 0;
	if (plan->count == plan->capacity) {
		if (plan->capacity > INT_MAX / 2)
			return 0;
		capacity = plan->capacity ? plan->capacity * 2 : 128;
		resized = realloc(plan->entries,
			(size_t)capacity * sizeof(plan->entries[0]));
		if (!resized)
			return 0;
		plan->entries = resized;
		plan->capacity = capacity;
	}
	entry = &plan->entries[plan->count];
	memset(entry, 0, sizeof(*entry));
	snprintf(entry->name, sizeof(entry->name), "%s", package);
	plan->count++;
	return 1;
}

static void write_log_line(FILE *log, const char *line)
{
	if (!log || !line)
		return;
	fprintf(log, "%s\n", line);
	fflush(log);
}

static void plan_line(const char *line, void *opaque)
{
	struct package_plan *plan = opaque;
	char package[NETWORK_PACKAGE_NAME_SIZE];
	write_log_line(plan ? plan->log : NULL, line);
	if (plan && package_action_line(line) &&
		action_package(line, package, sizeof(package)))
		plan_add(plan, package);
}

static int plan_counts(const struct package_plan *plan, int *installed,
	int *remaining, int *skipped)
{
	int installed_count = 0;
	int remaining_count = 0;
	int skipped_count = 0;

	if (!plan)
		return 0;
	for (int i = 0; i < plan->count; ++i) {
		if (plan->entries[i].installed)
			installed_count++;
		else if (plan->entries[i].skipped)
			skipped_count++;
		else
			remaining_count++;
	}
	if (installed)
		*installed = installed_count;
	if (remaining)
		*remaining = remaining_count;
	if (skipped)
		*skipped = skipped_count;
	return plan->count;
}

static void installed_line(const char *line, void *opaque)
{
	struct package_plan *plan = opaque;
	char package[NETWORK_PACKAGE_NAME_SIZE];
	int index;

	if (!plan || !copy_package_token(line, package, sizeof(package)))
		return;
	index = plan_find(plan, package);
	if (index >= 0)
		plan->entries[index].installed = 1;
}

static void refresh_installed(char *opkg, struct package_plan *plan)
{
	char *argv[] = {opkg, "list-installed", NULL};
	if (!plan)
		return;
	for (int i = 0; i < plan->count; ++i)
		plan->entries[i].installed = 0;
	process_run(argv, NULL, installed_line, plan);
}

static void reset_action_marks(struct package_plan *plan)
{
	if (!plan)
		return;
	for (int i = 0; i < plan->count; ++i)
		plan->entries[i].seen_action = 0;
}

static void show_install_state(struct install_state *state)
{
	char detail[512];
	long elapsed;
	if (!state)
		return;
	elapsed = (long)(time(NULL) - state->started);
	if (elapsed < 0)
		elapsed = 0;
	if (state->phase == INSTALL_PHASE_UPDATE)
		snprintf(detail, sizeof(detail), "Feed update - %ld s - %.220s",
			elapsed, state->line[0] ? state->line : "Waiting for data...");
	else if (state->planned > 0)
		snprintf(detail, sizeof(detail),
			"Installed before this run: %d / %d - Remaining: %d - "
			"Actions: %d / %d - Skipped: %d - %ld s - %.150s",
			state->verified_installed,
			state->plan ? state->plan->count : state->planned,
			state->planned, state->completed, state->planned,
			state->skipped, elapsed,
			state->line[0] ? state->line : "Resolving downloads...");
	else
		snprintf(detail, sizeof(detail), "Package operation - %ld s - %.220s",
			elapsed, state->line[0] ? state->line : "Resolving downloads...");
	progress(state->callback, state->opaque, state->percent, detail);
}

static void install_tick(void *opaque)
{
	show_install_state(opaque);
}

static void record_failure_line(struct install_state *state, const char *line)
{
	char package[NETWORK_PACKAGE_NAME_SIZE];
	int score = 0;

	if (error_package(line, package, sizeof(package))) {
		score = failure_package_score(state->plan, package);
	} else if (state->current_package[0]) {
		snprintf(package, sizeof(package), "%s", state->current_package);
		score = 2;
	}
	if (score >= state->failure_score && score > 0) {
		snprintf(state->failure_package, sizeof(state->failure_package), "%s",
			package);
		state->failure_score = score;
		snprintf(state->failure_detail, sizeof(state->failure_detail), "%s",
			line);
	} else if (!state->failure_detail[0] &&
		strcmp(line, "Collected errors:") != 0) {
		snprintf(state->failure_detail, sizeof(state->failure_detail), "%s",
			line);
	}
}

static void advance_package_action(struct install_state *state)
{
	int index = plan_find(state->plan, state->current_package);
	int calculated;

	if (index >= 0 && !state->plan->entries[index].seen_action) {
		state->plan->entries[index].seen_action = 1;
		state->completed++;
	}
	if (state->planned > 0) {
		calculated = 45 + state->completed * 45 / state->planned;
		if (calculated > 92)
			calculated = 92;
		if (calculated > state->percent)
			state->percent = calculated;
	} else if (state->percent < 90) {
		state->percent += 2;
	}
}

static void install_line(const char *line, void *opaque)
{
	struct install_state *state = opaque;
	char package[NETWORK_PACKAGE_NAME_SIZE];

	if (!state || !line)
		return;
	write_log_line(state->log, line);
	snprintf(state->line, sizeof(state->line), "%s", line);
	if (action_package(line, package, sizeof(package)))
		snprintf(state->current_package, sizeof(state->current_package), "%s",
			package);
	if (error_line(line))
		record_failure_line(state, line);
	if (state->phase == INSTALL_PHASE_UPDATE) {
		if (state->percent < 32)
			state->percent++;
	} else if (package_action_line(line)) {
		advance_package_action(state);
	} else if (strncmp(line, "Downloading ", 12) == 0 &&
		state->percent < 45) {
		state->percent = 45;
	} else if (strncmp(line, "Configuring ", 12) == 0) {
		if (state->percent < 92)
			state->percent = 92;
		else if (state->percent < 97)
			state->percent++;
	}
}

static void prepare_install_state(struct install_state *state,
	struct package_plan *plan, int installed, int remaining, int skipped,
	int recovery)
{
	reset_action_marks(plan);
	state->percent = recovery && plan->count > 0 ?
		45 + installed * 45 / plan->count : 40;
	state->phase = INSTALL_PHASE_PACKAGES;
	state->planned = remaining;
	state->completed = 0;
	state->verified_installed = installed;
	state->skipped = skipped;
	state->started = time(NULL);
	state->line[0] = '\0';
	state->current_package[0] = '\0';
	state->failure_package[0] = '\0';
	state->failure_detail[0] = '\0';
	state->failure_score = 0;
}

static int run_group_install(char *opkg, char *cache_option,
	struct install_state *state)
{
	char *argv[] = {opkg, cache_option, OPKG_CACHE_DIR,
		"-t", OPKG_TMP_DIR, "-l", OPKG_LISTS_DIR, "install",
		"packagegroup-openatv-small", NULL};
	write_log_line(state->log, "--- Installing packagegroup-openatv-small ---");
	return process_run_with_updates(argv, NULL, install_line, install_tick,
		1000, state);
}

static int run_remaining_install(char *opkg, char *cache_option,
	struct package_plan *plan, struct install_state *state)
{
	char **argv;
	int argument_count = 9;
	int index = 0;
	int result;

	for (int i = 0; i < plan->count; ++i) {
		if (plan->entries[i].skipped && !plan->entries[i].installed)
			argument_count += 2;
		else if (!plan->entries[i].installed)
			argument_count++;
	}
	argv = calloc((size_t)argument_count + 1, sizeof(argv[0]));
	if (!argv)
		return -1;
	argv[index++] = opkg;
	argv[index++] = cache_option;
	argv[index++] = OPKG_CACHE_DIR;
	argv[index++] = "-t";
	argv[index++] = OPKG_TMP_DIR;
	argv[index++] = "-l";
	argv[index++] = OPKG_LISTS_DIR;
	argv[index++] = "--nodeps";
	for (int i = 0; i < plan->count; ++i) {
		if (plan->entries[i].skipped && !plan->entries[i].installed) {
			argv[index++] = "--add-exclude";
			argv[index++] = plan->entries[i].name;
		}
	}
	argv[index++] = "install";
	for (int i = 0; i < plan->count; ++i) {
		if (!plan->entries[i].installed && !plan->entries[i].skipped)
			argv[index++] = plan->entries[i].name;
	}
	argv[index] = NULL;
	write_log_line(state->log,
		"--- Recovering all remaining packages in one bulk operation ---");
	result = process_run_with_updates(argv, NULL, install_line, install_tick,
		1000, state);
	free(argv);
	return result;
}

static int mark_skipped(struct package_plan *plan, const char *package)
{
	int index;
	if (!plan || !package || !package[0])
		return 0;
	index = plan_find(plan, package);
	if (index < 0) {
		if (!plan_add(plan, package))
			return 0;
		index = plan_find(plan, package);
	}
	if (index < 0)
		return 0;
	if (plan->entries[index].skipped)
		return 1;
	plan->entries[index].skipped = 1;
	return 1;
}

static void dependent_line(const char *line, void *opaque)
{
	struct package_plan *plan = opaque;
	char package[NETWORK_PACKAGE_NAME_SIZE];
	int index;

	write_log_line(plan ? plan->log : NULL, line);
	if (!plan || !copy_package_token(line, package, sizeof(package)))
		return;
	index = plan_find(plan, package);
	if (index >= 0 && !plan->entries[index].installed)
		plan->entries[index].skipped = 1;
}

static void mark_dependent_packages_skipped(char *opkg,
	struct package_plan *plan, char *package)
{
	char *argv[] = {opkg, "-l", OPKG_LISTS_DIR,
		"whatdependsrec", "-A", package, NULL};
	write_log_line(plan ? plan->log : NULL,
		"--- Marking the unavailable package dependency chain as skipped ---");
	process_run(argv, NULL, dependent_line, plan);
}

static void load_skipped_packages(struct package_plan *plan)
{
	FILE *file;
	char line[NETWORK_PACKAGE_NAME_SIZE + 32];

	if (!plan)
		return;
	file = fopen(OPKG_SKIPPED_FILE, "r");
	if (!file)
		return;
	while (fgets(line, sizeof(line), file)) {
		char package[NETWORK_PACKAGE_NAME_SIZE];
		if (line[0] == '#' || !copy_package_token(line, package,
			sizeof(package)) || hexadecimal_hash(package, strlen(package)))
			continue;
		mark_skipped(plan, package);
	}
	fclose(file);
}

static const char *first_remaining_package(const struct package_plan *plan)
{
	if (!plan)
		return NULL;
	for (int i = 0; i < plan->count; ++i) {
		if (!plan->entries[i].installed && !plan->entries[i].skipped)
			return plan->entries[i].name;
	}
	return NULL;
}

static int save_skipped_packages(const struct package_plan *plan)
{
	FILE *file;
	int count = 0;
	int ok = 1;
	int i;
	for (i = 0; plan && i < plan->count; ++i) {
		if (plan->entries[i].skipped && !plan->entries[i].installed)
			count++;
	}
	if (!count) {
		unlink(OPKG_SKIPPED_FILE);
		return 1;
	}
	file = fopen(OPKG_SKIPPED_FILE, "w");
	if (!file)
		return 0;
	fputs("# Packages skipped by SmallBox Wizard. Install them after fixing the feed.\n",
		file);
	for (i = 0; i < plan->count; ++i) {
		if (plan->entries[i].skipped && !plan->entries[i].installed)
			fprintf(file, "%s\n", plan->entries[i].name);
	}
	if (fflush(file) != 0)
		ok = 0;
	if (fsync(fileno(file)) != 0)
		ok = 0;
	if (fclose(file) != 0)
		ok = 0;
	return ok;
}

struct install_run {
	char opkg[256];
	char cache_option[sizeof("--cache-dir")];
	struct package_plan plan;
	struct install_state state;
	FILE *log;
	network_progress_cb callback;
	network_package_failure_cb failure_callback;
	void *opaque;
	char *error;
	size_t error_size;
	int installed;
	int remaining;
	int skipped;
};

static int create_work_directories(void)
{
	return ensure_directory("/.FlashExpander", 0755) &&
		ensure_directory(OPKG_WORK_ROOT, 0755) &&
		ensure_directory(OPKG_CACHE_DIR, 0755) &&
		ensure_directory(OPKG_TMP_DIR, 0755) &&
		ensure_directory(OPKG_LISTS_DIR, 0755);
}

static int finish_install(struct install_run *run, int result)
{
	fclose(run->log);
	free(run->plan.entries);
	return result;
}

static void detect_cache_option(struct install_run *run)
{
	char help[4096];
	char *argv[] = {run->opkg, "--help", NULL};

	snprintf(run->cache_option, sizeof(run->cache_option), "%s", "--cache");
	process_capture(argv, help, sizeof(help));
	if (strstr(help, "--cache-dir"))
		snprintf(run->cache_option, sizeof(run->cache_option), "%s",
			"--cache-dir");
}

static int update_package_lists(struct install_run *run)
{
	char *argv[] = {run->opkg, run->cache_option, OPKG_CACHE_DIR,
		"-t", OPKG_TMP_DIR, "-l", OPKG_LISTS_DIR, "update", NULL};
	int result;

	run->state.percent = 5;
	run->state.phase = INSTALL_PHASE_UPDATE;
	run->state.started = time(NULL);
	progress(run->callback, run->opaque, 5, "Updating package lists...");
	write_log_line(run->log, "--- Updating package lists ---");
	result = process_run_with_updates(argv, NULL, install_line,
		install_tick, 1000, &run->state);
	if (result != 0) {
		set_error(run->error, run->error_size,
			"opkg update failed with status %d. %.300s Full log: %s",
			result, run->state.failure_detail[0] ? run->state.failure_detail :
			"Check the network and package feeds.", OPKG_LOG_FILE);
		return 0;
	}
	return 1;
}

static int resolve_package_plan(struct install_run *run)
{
	char *argv[] = {run->opkg, run->cache_option, OPKG_CACHE_DIR,
		"-t", OPKG_TMP_DIR, "-l", OPKG_LISTS_DIR,
		"--noaction", "install", "packagegroup-openatv-small", NULL};
	int result;

	progress(run->callback, run->opaque, 35, "Resolving package dependencies...");
	write_log_line(run->log, "--- Resolving the package plan ---");
	result = process_run(argv, NULL, plan_line, &run->plan);
	if (result != 0) {
		set_error(run->error, run->error_size,
			"The SmallBox package plan cannot be resolved (status %d). Full log: %s",
			result, OPKG_LOG_FILE);
		return 0;
	}
	return 1;
}

static void refresh_plan_counts(struct install_run *run)
{
	refresh_installed(run->opkg, &run->plan);
	plan_counts(&run->plan, &run->installed, &run->remaining, &run->skipped);
}

static int run_install(struct install_run *run)
{
	if (run->skipped)
		return run_remaining_install(run->opkg, run->cache_option, &run->plan,
			&run->state);
	return run_group_install(run->opkg, run->cache_option, &run->state);
}

static int start_install(struct install_run *run)
{
	char detail[192];

	load_skipped_packages(&run->plan);
	refresh_plan_counts(run);
	prepare_install_state(&run->state, &run->plan, run->installed,
		run->remaining, run->skipped, 0);
	snprintf(detail, sizeof(detail),
		"Installing %d packages and dependencies in one bulk operation...",
		run->remaining);
	progress(run->callback, run->opaque, 40, detail);
	if (run->remaining == 0 && run->skipped)
		return 0;
	return run_install(run);
}

static const char *failure_package_name(const struct install_run *run)
{
	const char *fallback_package = first_remaining_package(&run->plan);

	if (run->state.failure_package[0])
		return run->state.failure_package;
	if (run->state.current_package[0])
		return run->state.current_package;
	return fallback_package ? fallback_package : "";
}

static const char *failure_detail_text(const struct install_run *run, int result)
{
	if (run->state.failure_detail[0])
		return run->state.failure_detail;
	if (result == 0)
		return "OPKG returned success, but packages are still missing.";
	return "OPKG stopped without a detailed error line. See the full log.";
}

static void describe_failure(const struct install_run *run, int result,
	struct network_package_failure *failure)
{
	memset(failure, 0, sizeof(*failure));
	failure->status = result != 0 ? result : 255;
	failure->total = run->plan.count;
	failure->installed = run->installed;
	failure->remaining = run->remaining;
	failure->skipped = run->skipped;
	snprintf(failure->package, sizeof(failure->package), "%s",
		failure_package_name(run));
	snprintf(failure->detail, sizeof(failure->detail), "%s",
		failure_detail_text(run, result));
}

static int skip_failed_package(struct install_run *run,
	struct network_package_failure *failure)
{
	char detail[256];

	if (!failure->package[0] || run->skipped >= MAX_RECOVERY_SKIPS ||
		!mark_skipped(&run->plan, failure->package)) {
		set_error(run->error, run->error_size,
			"The failed package cannot be skipped safely. Full log: %s",
			OPKG_LOG_FILE);
		return 0;
	}
	write_log_line(run->log, "--- User selected: skip package and continue ---");
	snprintf(detail, sizeof(detail),
		"Checking which remaining packages depend on %.120s...",
		failure->package);
	progress(run->callback, run->opaque, run->state.percent, detail);
	mark_dependent_packages_skipped(run->opkg, &run->plan, failure->package);
	if (!save_skipped_packages(&run->plan)) {
		set_error(run->error, run->error_size,
			"The skipped-package list cannot be saved: %s",
			strerror(errno));
		return 0;
	}
	return 1;
}

static int recover_until_complete(struct install_run *run, int result)
{
	for (;;) {
		struct network_package_failure failure;
		enum network_package_action action = NETWORK_PACKAGE_ABORT;

		refresh_plan_counts(run);
		if (result == 0 && run->remaining == 0)
			return 1;
		describe_failure(run, result, &failure);
		if (run->failure_callback)
			action = run->failure_callback(&failure, run->opaque);
		if (action == NETWORK_PACKAGE_ABORT) {
			set_error(run->error, run->error_size,
				"OPKG status %d. Package: %s. Installed %d/%d; %d remain. "
				"%.180s Full log: %s", failure.status,
				failure.package[0] ? failure.package : "not detected",
				run->installed, run->plan.count, run->remaining, failure.detail,
				OPKG_LOG_FILE);
			return 0;
		}
		if (action == NETWORK_PACKAGE_SKIP && !skip_failed_package(run, &failure))
			return 0;
		refresh_plan_counts(run);
		prepare_install_state(&run->state, &run->plan, run->installed,
			run->remaining, run->skipped, 1);
		if (run->remaining == 0)
			result = 0;
		else
			result = run_install(run);
	}
}

static void clean_and_report(struct install_run *run)
{
	char detail[256];
	char *argv[] = {run->opkg, run->cache_option, OPKG_CACHE_DIR,
		"-t", OPKG_TMP_DIR, "-l", OPKG_LISTS_DIR, "clean", NULL};

	snprintf(detail, sizeof(detail),
		"Installed %d of %d packages; skipped %d. Cleaning the external OPKG cache...",
		run->installed, run->plan.count, run->skipped);
	progress(run->callback, run->opaque, 98, detail);
	process_run(argv, NULL, NULL, NULL);
	if (run->skipped) {
		snprintf(detail, sizeof(detail),
			"Installation completed with %d skipped package(s). The list is in %s.",
			run->skipped, OPKG_SKIPPED_FILE);
		progress(run->callback, run->opaque, 100, detail);
	} else {
		progress(run->callback, run->opaque, 100,
			"The SmallBox packages have been installed.");
	}
}

int network_install_smallbox(network_progress_cb callback,
	network_package_failure_cb failure_callback, void *opaque,
	char *error, size_t error_size)
{
	struct install_run run;

	memset(&run, 0, sizeof(run));
	run.callback = callback;
	run.failure_callback = failure_callback;
	run.opaque = opaque;
	run.error = error;
	run.error_size = error_size;
	if (error && error_size)
		error[0] = '\0';
	if (!process_find("opkg", run.opkg, sizeof(run.opkg))) {
		set_error(error, error_size, "opkg is not installed in this image.");
		return 0;
	}
	if (!create_work_directories()) {
		set_error(error, error_size,
			"The OPKG work directories cannot be created on the FlashExpander.");
		return 0;
	}
	run.log = fopen(OPKG_LOG_FILE, "w");
	if (!run.log) {
		set_error(error, error_size, "The OPKG log cannot be created: %s",
			strerror(errno));
		return 0;
	}
	run.plan.log = run.log;
	run.state.callback = callback;
	run.state.opaque = opaque;
	run.state.plan = &run.plan;
	run.state.log = run.log;
	detect_cache_option(&run);
	if (!update_package_lists(&run) || !resolve_package_plan(&run) ||
		!recover_until_complete(&run, start_install(&run)))
		return finish_install(&run, 0);
	if (!save_skipped_packages(&run.plan)) {
		set_error(error, error_size,
			"The skipped-package list cannot be saved: %s", strerror(errno));
		return finish_install(&run, 0);
	}
	clean_and_report(&run);
	return finish_install(&run, 1);
}

static int service_link(const char *name, const char *service)
{
	size_t name_length;
	size_t service_length;

	if (!name || !service || name[0] != 'S')
		return 0;
	name_length = strlen(name);
	service_length = strlen(service);
	return name_length >= service_length &&
		strcmp(name + name_length - service_length, service) == 0;
}

static int optional_service_link(const char *name)
{
	static const char *const services[] = {
		"autofs",
		"avahi-daemon",
		"llmnrd",
		"nfsserver",
		"samba",
		"smartd",
		"telnetd.busybox",
		"vsftpd",
		"wsdd",
		"wsdd2",
		NULL
	};

	for (int index = 0; services[index]; ++index) {
		if (service_link(name, services[index]))
			return 1;
	}
	return 0;
}

static int disable_service_link(const char *directory_path, const char *name,
	char *error, size_t error_size)
{
	char path[PATH_MAX];

	if (snprintf(path, sizeof(path), "%s/%s", directory_path, name) >=
		(int)sizeof(path)) {
		set_error(error, error_size, "An optional service link path is too long.");
		return 0;
	}
	if (unlink(path) != 0 && errno != ENOENT) {
		set_error(error, error_size, "%s cannot be disabled: %s", path,
			strerror(errno));
		return 0;
	}
	return 1;
}

/* -1 on error, 0 if the runlevel directory is missing, 1 when done. */
static int disable_runlevel_services(int runlevel, int *removed, char *error,
	size_t error_size)
{
	char directory_path[64];
	DIR *directory;
	const struct dirent *entry;

	snprintf(directory_path, sizeof(directory_path), "/etc/rc%d.d", runlevel);
	directory = opendir(directory_path);
	if (!directory) {
		if (errno == ENOENT)
			return 0;
		set_error(error, error_size, "%s cannot be inspected: %s",
			directory_path, strerror(errno));
		return -1;
	}
	for (;;) {
		errno = 0;
		entry = readdir(directory);
		if (!entry)
			break;
		if (!optional_service_link(entry->d_name))
			continue;
		if (!disable_service_link(directory_path, entry->d_name, error,
			error_size)) {
			closedir(directory);
			return -1;
		}
		(*removed)++;
	}
	if (errno != 0) {
		int saved_errno = errno;
		closedir(directory);
		set_error(error, error_size, "%s cannot be read: %s",
			directory_path, strerror(saved_errno));
		return -1;
	}
	closedir(directory);
	return 1;
}

int network_disable_optional_services(network_progress_cb callback,
	void *opaque, char *error, size_t error_size)
{
	int removed = 0;

	if (error && error_size)
		error[0] = '\0';
	progress(callback, opaque, 5,
		"Disabling optional background services for this single-core receiver...");
	for (int runlevel = 2; runlevel <= 5; ++runlevel) {
		int status = disable_runlevel_services(runlevel, &removed, error,
			error_size);
		if (status < 0)
			return 0;
		if (status > 0)
			progress(callback, opaque, (runlevel - 1) * 25,
				"Applying the TV-focused single-core boot profile...");
	}
	{
		char detail[192];
		snprintf(detail, sizeof(detail),
			"Single-core boot profile ready. Disabled %d optional startup link(s).",
			removed);
		progress(callback, opaque, 100, detail);
	}
	return 1;
}
