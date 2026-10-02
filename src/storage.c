#define _GNU_SOURCE

#include "storage.h"
#include "process.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define EXPANDER_MOUNT "/.FlashExpander"
#define EXPANDER_USR "/.FlashExpander/.FlashExpander"
#define EXPANDER_META "/.FlashExpander/.smallbox-wizard"
#define EXPANDER_USR_REL ".FlashExpander"
#define EXPANDER_META_REL ".smallbox-wizard"
#define FSTAB_BEGIN "# SMALLBOX-WIZARD BEGIN"
#define FSTAB_END "# SMALLBOX-WIZARD END"
#define SWAP_BYTES (512ULL * 1024ULL * 1024ULL)
#define MIN_DATA_BYTES (512ULL * 1024ULL * 1024ULL)

__attribute__((format(printf, 3, 4)))
static void set_error(char *error, size_t error_size, const char *format, ...)  /* NOSONAR printf-style helper */
{
	va_list arguments;
	if (!error || error_size == 0)
		return;
	va_start(arguments, format);
	vsnprintf(error, error_size, format, arguments);  /* NOSONAR format checked by attribute */
	va_end(arguments);
}

static void sleep_ms(long milliseconds)
{
	struct timespec delay;
	delay.tv_sec = milliseconds / 1000L;
	delay.tv_nsec = (milliseconds % 1000L) * 1000000L;
	nanosleep(&delay, NULL);
}

static void trim(char *text)
{
	size_t length;
	const char *start = text;
	if (!text)
		return;
	while (*start && isspace((unsigned char)*start))  /* NOSONAR stops at the terminator */
		start++;
	if (start != text)
		memmove(text, start, strlen(start) + 1);
	length = strlen(text);
	while (length && isspace((unsigned char)text[length - 1]))
		text[--length] = '\0';
}

static int read_text(const char *path, char *text, size_t text_size)
{
	FILE *file;
	if (!text || text_size == 0)
		return 0;
	text[0] = '\0';
	file = fopen(path, "r");
	if (!file)
		return 0;
	if (!fgets(text, (int)text_size, file)) {
		fclose(file);
		return 0;
	}
	fclose(file);
	trim(text);
	return 1;
}

static int is_disk_name(const char *name)
{
	if (!name || name[0] != 's' || name[1] != 'd' ||
		!isalpha((unsigned char)name[2]))
		return 0;
	for (const char *cursor = name + 2; *cursor; ++cursor) {
		if (!isalpha((unsigned char)*cursor))
			return 0;
	}
	return 1;
}

static const char *block_sysfs_root(void)
{
	struct stat status;
	if (stat("/sys/class/block", &status) == 0 && S_ISDIR(status.st_mode))
		return "/sys/class/block";
	return "/sys/block";
}

static int path_is_usb(const char *sysfs_root, const char *name)
{
	char link_path[PATH_MAX];
	char resolved[PATH_MAX];
	snprintf(link_path, sizeof(link_path), "%s/%s/device", sysfs_root, name);
	if (!realpath(link_path, resolved))
		return 0;
	return strstr(resolved, "/usb") != NULL;
}

struct dev_numbers {
	char values[8][32];
	int count;
};

static void read_critical_devices(struct dev_numbers *numbers)
{
	FILE *file;
	char line[1024];
	memset(numbers, 0, sizeof(*numbers));
	file = fopen("/proc/self/mountinfo", "r");
	if (!file)
		return;
	while (fgets(line, sizeof(line), file) && numbers->count < 8) {
		char device[32];
		char mountpoint[PATH_MAX];
		if (sscanf(line, "%*s %*s %31s %*s %4095s", device,
			mountpoint) != 2)
			continue;
		if (strcmp(mountpoint, "/") == 0 ||
			strcmp(mountpoint, "/usr") == 0 ||
			strcmp(mountpoint, "/boot") == 0) {
			snprintf(numbers->values[numbers->count],
				sizeof(numbers->values[numbers->count]), "%s", device);
			numbers->count++;
		}
	}
	fclose(file);
}

static int matches_critical(const char *dev, const struct dev_numbers *numbers)
{
	for (int i = 0; i < numbers->count; ++i) {
		if (strcmp(dev, numbers->values[i]) == 0)
			return 1;
	}
	return 0;
}

static int disk_contains_critical(const char *sysfs_root, const char *disk,
	const struct dev_numbers *numbers)
{
	DIR *directory;
	struct dirent *entry;
	char path[PATH_MAX];
	char partition_directory[128];
	char dev[32];

	snprintf(path, sizeof(path), "%s/%.31s/dev", sysfs_root, disk);
	if (read_text(path, dev, sizeof(dev)) && matches_critical(dev, numbers))
		return 1;
	if (strcmp(sysfs_root, "/sys/block") == 0)
		snprintf(partition_directory, sizeof(partition_directory), "%s/%.31s",
			sysfs_root, disk);
	else
		snprintf(partition_directory, sizeof(partition_directory), "%s",
			sysfs_root);
	directory = opendir(partition_directory);
	if (!directory)
		return 0;
	while ((entry = readdir(directory)) != NULL) {
		char partition[32];
		if (strncmp(entry->d_name, disk, strlen(disk)) != 0 ||
			strcmp(entry->d_name, disk) == 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s/partition",
			partition_directory, entry->d_name);
		if (!read_text(path, partition, sizeof(partition)))
			continue;
		snprintf(path, sizeof(path), "%s/%s/dev", partition_directory,
			entry->d_name);
		if (read_text(path, dev, sizeof(dev)) &&
			matches_critical(dev, numbers)) {
			closedir(directory);
			return 1;
		}
	}
	closedir(directory);
	return 0;
}

static int device_compare(const void *left, const void *right)
{
	const struct storage_device *a = left;
	const struct storage_device *b = right;
	return strcmp(a->name, b->name);
}

int storage_scan_usb(struct storage_device devices[], int maximum,
	char *error, size_t error_size)
{
	DIR *directory;
	struct dirent *entry;
	struct dev_numbers critical;
	const char *sysfs_root;
	int count = 0;

	if (!devices || maximum <= 0) {
		set_error(error, error_size, "Invalid output buffer.");
		return -1;
	}
	read_critical_devices(&critical);
	sysfs_root = block_sysfs_root();
	directory = opendir(sysfs_root);
	if (!directory) {
		set_error(error, error_size, "Block devices cannot be read: %s",
			strerror(errno));
		return -1;
	}
	while ((entry = readdir(directory)) != NULL && count < maximum) {
		struct storage_device *device;
		char sys_path[PATH_MAX];
		char value[128];
		char *end = NULL;
		size_t name_length;
		unsigned long long sectors;
		struct stat status;

		if (!is_disk_name(entry->d_name) ||
			!path_is_usb(sysfs_root, entry->d_name) ||
			disk_contains_critical(sysfs_root, entry->d_name, &critical))
			continue;
		device = &devices[count];
		memset(device, 0, sizeof(*device));
		name_length = strnlen(entry->d_name, sizeof(device->name));
		if (name_length == sizeof(device->name))
			continue;
		memcpy(device->name, entry->d_name, name_length + 1);
		snprintf(device->path, sizeof(device->path), "/dev/%s", device->name);
		if (stat(device->path, &status) < 0 || !S_ISBLK(status.st_mode))
			continue;
		snprintf(sys_path, sizeof(sys_path), "%s/%s/size", sysfs_root,
			entry->d_name);
		if (!read_text(sys_path, value, sizeof(value)))
			continue;
		errno = 0;
		sectors = strtoull(value, &end, 10);
		if (errno || end == value || sectors == 0)
			continue;
		device->size_bytes = (uint64_t)sectors * 512ULL;
		if (device->size_bytes < SWAP_BYTES + MIN_DATA_BYTES)
			continue;
		snprintf(sys_path, sizeof(sys_path), "%s/%s/removable", sysfs_root,
			entry->d_name);
		device->removable = read_text(sys_path, value, sizeof(value)) &&
			atoi(value) != 0;
		snprintf(sys_path, sizeof(sys_path), "%s/%s/device/model", sysfs_root,
			entry->d_name);
		if (!read_text(sys_path, device->model, sizeof(device->model)))
			snprintf(device->model, sizeof(device->model), "USB storage");
		count++;
	}
	closedir(directory);
	qsort(devices, (size_t)count, sizeof(devices[0]), device_compare);
	if (error && error_size)
		error[0] = '\0';
	return count;
}

void storage_format_size(uint64_t bytes, char *text, size_t text_size)
{
	double value;
	const char *unit;
	if (bytes >= 1000ULL * 1000ULL * 1000ULL) {
		value = (double)bytes / (1000.0 * 1000.0 * 1000.0);
		unit = "GB";
	} else {
		value = (double)bytes / (1000.0 * 1000.0);
		unit = "MB";
	}
	snprintf(text, text_size, "%.1f %s", value, unit);
}

const char *storage_expander_mount(void)
{
	return EXPANDER_MOUNT;
}

static int read_meta_value(const char *key, char *value, size_t value_size)
{
	FILE *file;
	char line[256];
	size_t key_length;
	if (!key || !value || value_size == 0)
		return 0;
	value[0] = '\0';
	file = fopen(EXPANDER_META, "r");
	if (!file)
		return 0;
	key_length = strlen(key);
	while (fgets(line, sizeof(line), file)) {
		if (strncmp(line, key, key_length) == 0 &&
			line[key_length] == '=') {
			snprintf(value, value_size, "%s", line + key_length + 1);
			trim(value);
			fclose(file);
			return value[0] != '\0';
		}
	}
	fclose(file);
	return 0;
}

static int read_meta_uuid(char *uuid, size_t uuid_size)
{
	return read_meta_value("uuid", uuid, uuid_size);
}

static int usr_is_mountpoint(void)
{
	FILE *file = fopen("/proc/self/mountinfo", "r");
	char line[1024];
	if (!file)
		return 0;
	while (fgets(line, sizeof(line), file)) {
		char mountpoint[PATH_MAX];
		if (sscanf(line, "%*s %*s %*s %*s %4095s", mountpoint) == 1 &&
			strcmp(mountpoint, "/usr") == 0) {
			fclose(file);
			return 1;
		}
	}
	fclose(file);
	return 0;
}

int storage_is_expander_active(char *uuid, size_t uuid_size)
{
	struct stat internal;
	struct stat external;
	if (uuid && uuid_size)
		uuid[0] = '\0';
	if (!usr_is_mountpoint() || stat("/usr", &internal) < 0 ||
		stat(EXPANDER_USR, &external) < 0 ||
		internal.st_dev != external.st_dev ||
		access(EXPANDER_USR "/bin", F_OK) < 0)
		return 0;
	if (uuid && uuid_size)
		read_meta_uuid(uuid, uuid_size);
	return 1;
}

static void partition_path(const struct storage_device *device, int number,
	char *path, size_t path_size)
{
	size_t length = strlen(device->name);
	snprintf(path, path_size, "/dev/%s%s%d", device->name,
		length && isdigit((unsigned char)device->name[length - 1]) ? "p" : "",  /* NOSONAR length >= 1 here */
		number);
}

static int source_is_partition(const char *source, const char *device_path)
{
	size_t length = strlen(device_path);
	char next;
	if (strncmp(source, device_path, length) != 0)
		return 0;
	next = source[length];  /* NOSONAR prefix matched, index <= strlen(source) */
	return next == '\0' || isdigit((unsigned char)next) || next == 'p';  /* NOSONAR the character read above */
}

static void run_quiet(char *const argv[])
{
	process_run(argv, NULL, NULL, NULL);
}

static void swapoff_target(const struct storage_device *device)
{
	FILE *file;
	char line[1024];

	file = fopen("/proc/swaps", "r");
	if (!file)
		return;
	if (fgets(line, sizeof(line), file)) {
		while (fgets(line, sizeof(line), file)) {
			char source[PATH_MAX];
			if (sscanf(line, "%4095s", source) == 1 &&
				source_is_partition(source, device->path)) {
				char *argv[] = {"swapoff", source, NULL};
				run_quiet(argv);
			}
		}
	}
	fclose(file);
}

static int deactivate_target(const struct storage_device *device,
	char *error, size_t error_size)
{
	FILE *file;
	char line[1024];
	char sources[32][PATH_MAX];
	int count = 0;

	swapoff_target(device);
	file = fopen("/proc/mounts", "r");
	if (!file)
		return 1;
	while (fgets(line, sizeof(line), file) && count < 32) {
		char source[PATH_MAX];
		char mountpoint[PATH_MAX];
		if (sscanf(line, "%4095s %4095s", source, mountpoint) != 2 ||
			!source_is_partition(source, device->path))
			continue;
		if (strcmp(mountpoint, "/") == 0 || strcmp(mountpoint, "/usr") == 0 ||
			strcmp(mountpoint, "/boot") == 0) {
			fclose(file);
			set_error(error, error_size,
				"The target device contains an active system mount (%s).", mountpoint);
			return 0;
		}
		snprintf(sources[count++], sizeof(sources[0]), "%s", mountpoint);
	}
	fclose(file);
	for (int i = count - 1; i >= 0; --i) {
		char *argv[] = {"umount", sources[i], NULL};
		if (process_run(argv, NULL, NULL, NULL) != 0) {
			set_error(error, error_size, "A mount point could not be unmounted: %s",
				sources[i]);
			return 0;
		}
	}
	return 1;
}

static int validate_selected(const struct storage_device *selected)
{
	struct storage_device devices[STORAGE_MAX_DEVICES];
	char error[128];
	int count;
	count = storage_scan_usb(devices, STORAGE_MAX_DEVICES, error, sizeof(error));
	for (int i = 0; i < count; ++i) {
		if (strcmp(devices[i].path, selected->path) == 0 &&
			devices[i].size_bytes == selected->size_bytes)
			return 1;
	}
	return 0;
}

static int wait_for_partitions(const char *part1, const char *part2)
{
	struct stat status1;
	struct stat status2;
	for (int attempt = 0; attempt < 100; ++attempt) {
		if (stat(part1, &status1) == 0 && S_ISBLK(status1.st_mode) &&
			stat(part2, &status2) == 0 && S_ISBLK(status2.st_mode))
			return 1;
		sleep_ms(100);
	}
	return 0;
}

static int make_directory(const char *path, mode_t mode)
{
	if (mkdir(path, mode) == 0 || errno == EEXIST)
		return 1;
	return 0;
}

static int create_nomount_flag(const struct storage_device *device,
	char *path, size_t path_size)
{
	int fd;
	if (!device || !path || path_size == 0)
		return 0;
	if (snprintf(path, path_size, "/dev/nomount.%s", device->name) >=
		(int)path_size)
		return 0;
	fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);  /* NOSONAR /dev/nomount.<sysfs disk name> */
	if (fd < 0)
		return 0;
	close(fd);
	return 1;
}

static int blkid_uuid(const char *partition, char *uuid, size_t uuid_size)
{
	char blkid[PATH_MAX];
	char target[PATH_MAX];
	char *argv[] = {blkid, "-s", "UUID", "-o", "value", target, NULL};
	if (snprintf(target, sizeof(target), "%s", partition) >= (int)sizeof(target))
		return 0;
	if (!process_find("blkid", blkid, sizeof(blkid)))
		return 0;
	if (process_capture(argv, uuid, uuid_size) != 0)
		return 0;
	trim(uuid);
	return uuid[0] != '\0';
}

static int copy_mode(const char *path, mode_t *mode)
{
	struct stat status;
	if (stat(path, &status) < 0)
		return 0;
	*mode = status.st_mode & 0777;
	return 1;
}

/* Flush, sync, close and rename; the stream is always closed, errno kept. */
static int commit_file(FILE *file, int fd, const char *temporary,
	const char *target)
{
	int ok = fflush(file) == 0 && fsync(fd) == 0;
	int saved_errno = errno;
	if (fclose(file) != 0 && ok) {
		ok = 0;
		saved_errno = errno;
	}
	if (ok && rename(temporary, target) != 0) {
		ok = 0;
		saved_errno = errno;
	}
	errno = saved_errno;
	return ok;
}

static int update_fstab(const char *data_uuid, const char *swap_uuid,
	int enable, char *error, size_t error_size)
{
	FILE *input;
	FILE *output;
	char temporary[] = "/etc/fstab.smallbox.XXXXXX";
	char line[1024];
	int skip = 0;
	int fd;
	mode_t mode = 0644;

	input = fopen("/etc/fstab", "r");
	copy_mode("/etc/fstab", &mode);
	fd = mkstemp(temporary);
	if (fd < 0) {
		set_error(error, error_size, "fstab cannot be prepared: %s",
			strerror(errno));
		if (input) fclose(input);
		return 0;
	}
	fchmod(fd, mode);
	output = fdopen(fd, "w");
	if (!output) {
		close(fd);
		unlink(temporary);
		if (input) fclose(input);
		set_error(error, error_size, "fstab cannot be written.");
		return 0;
	}
	if (input) {
		while (fgets(line, sizeof(line), input)) {
			if (strncmp(line, FSTAB_BEGIN, strlen(FSTAB_BEGIN)) == 0) {
				skip = 1;
				continue;
			}
			if (skip && strncmp(line, FSTAB_END, strlen(FSTAB_END)) == 0) {
				skip = 0;
				continue;
			}
			if (!skip)
				fputs(line, output);
		}
		fclose(input);
	}
	if (enable) {
		fprintf(output, "\n%s\n", FSTAB_BEGIN);
		fprintf(output, "UUID=%s %s ext4 defaults,noatime 0 2\n",
			data_uuid, EXPANDER_MOUNT);
		fprintf(output, "%s /usr none bind 0 0\n", EXPANDER_USR);
		fprintf(output, "UUID=%s none swap sw 0 0\n", swap_uuid);
		fprintf(output, "%s\n", FSTAB_END);
	}
	if (!commit_file(output, fd, temporary, "/etc/fstab")) {
		set_error(error, error_size, "fstab could not be saved: %s",
			strerror(errno));
		unlink(temporary);
		return 0;
	}
	return 1;
}

static int active_swap_for_uuid(const char *uuid, char *source,
	size_t source_size)
{
	FILE *file;
	char line[1024];
	if (!source || source_size == 0)
		return 0;
	source[0] = '\0';
	if (!uuid || !uuid[0])
		return 0;
	file = fopen("/proc/swaps", "r");
	if (!file)
		return 0;
	if (fgets(line, sizeof(line), file)) {
		while (fgets(line, sizeof(line), file)) {
			char candidate[PATH_MAX];
			char candidate_uuid[128];
			if (sscanf(line, "%4095s", candidate) == 1 &&
				blkid_uuid(candidate, candidate_uuid,
					sizeof(candidate_uuid)) &&
				strcmp(candidate_uuid, uuid) == 0) {
				snprintf(source, source_size, "%s", candidate);
				fclose(file);
				return 1;
			}
		}
	}
	fclose(file);
	return 0;
}

static void restore_expander(const char *swap_source, int data_unmounted)
{
	if (data_unmounted) {
		char *mount_data[] = {"mount", EXPANDER_MOUNT, NULL};
		run_quiet(mount_data);
	}
	{
		char *mount_usr[] = {"mount", "/usr", NULL};
		run_quiet(mount_usr);
	}
	if (swap_source && swap_source[0]) {
		char source[PATH_MAX];
		char *swapon[] = {"swapon", source, NULL};
		snprintf(source, sizeof(source), "%s", swap_source);
		run_quiet(swapon);
	}
}

int storage_reset_expander(char *error, size_t error_size)
{
	char data_uuid[128];
	char swap_uuid[128];
	char swap_source[PATH_MAX];
	int swap_disabled = 0;
	int data_unmounted = 0;

	if (error && error_size)
		error[0] = '\0';
	if (!storage_is_expander_active(data_uuid, sizeof(data_uuid))) {
		set_error(error, error_size,
			"No active FlashExpander setup was found.");
		return 0;
	}
	if (!read_meta_value("swap_uuid", swap_uuid, sizeof(swap_uuid)))
		swap_uuid[0] = '\0';
	active_swap_for_uuid(swap_uuid, swap_source, sizeof(swap_source));
	if (chdir("/") != 0) {
		set_error(error, error_size,
			"The wizard cannot release its USB working directory.");
		return 0;
	}
	{
		char *argv[] = {"umount", "/usr", NULL};
		if (process_run(argv, NULL, NULL, NULL) != 0) {
			set_error(error, error_size,
				"The active USB /usr mount could not be disconnected.");
			return 0;
		}
	}
	if (swap_source[0]) {
		char *argv[] = {"swapoff", swap_source, NULL};
		if (process_run(argv, NULL, NULL, NULL) != 0) {
			restore_expander(NULL, 0);
			set_error(error, error_size,
				"The active SmallBox swap could not be disabled.");
			return 0;
		}
		swap_disabled = 1;
	}
	{
		char *argv[] = {"umount", EXPANDER_MOUNT, NULL};
		if (process_run(argv, NULL, NULL, NULL) != 0) {
			restore_expander(swap_disabled ? swap_source : NULL, 0);
			set_error(error, error_size,
				"The active FlashExpander could not be disconnected.");
			return 0;
		}
		data_unmounted = 1;
	}
	if (!update_fstab(NULL, NULL, 0, error, error_size)) {
		restore_expander(swap_disabled ? swap_source : NULL,
			data_unmounted);
		return 0;
	}
	sync();
	return 1;
}

static int write_metadata(const char *data_uuid, const char *swap_uuid)
{
	char temporary[] = "./.smallbox-wizard.XXXXXX";
	int fd = mkstemp(temporary);
	FILE *file;
	if (fd < 0)
		return 0;
	file = fdopen(fd, "w");
	if (!file) {
		close(fd);
		unlink(temporary);
		return 0;
	}
	fprintf(file, "version=1\nuuid=%s\nswap_uuid=%s\n", data_uuid,
		swap_uuid);
	if (!commit_file(file, fd, temporary, EXPANDER_META_REL)) {
		unlink(temporary);
		return 0;
	}
	return 1;
}

struct copy_state {
	storage_progress_cb callback;
	void *opaque;
	uint64_t total_bytes;
	uint64_t initial_free_bytes;
	unsigned int elapsed_seconds;
	char error_line[256];
};

static uint64_t filesystem_free_bytes(const char *path)
{
	struct statvfs status;
	uint64_t block_size;
	if (statvfs(path, &status) != 0)
		return 0;
	block_size = status.f_frsize ? status.f_frsize : status.f_bsize;
	return (uint64_t)status.f_bfree * block_size;
}

static uint64_t directory_allocated_bytes(const char *path)
{
	char du[PATH_MAX];
	char output[128];
	char target[PATH_MAX];
	char *end;
	unsigned long long kibibytes;
	char *argv[] = {du, "-sk", target, NULL};
	if (snprintf(target, sizeof(target), "%s", path) >= (int)sizeof(target) ||
		!process_find("du", du, sizeof(du)) ||
		process_capture(argv, output, sizeof(output)) != 0)
		return 0;
	errno = 0;
	kibibytes = strtoull(output, &end, 10);
	if (errno != 0 || end == output)
		return 0;
	return (uint64_t)kibibytes * 1024ULL;
}

static void copy_line(const char *line, void *opaque)
{
	struct copy_state *state = opaque;
	if (state && line && line[0])
		snprintf(state->error_line, sizeof(state->error_line), "%s", line);
}

static void copy_tick(void *opaque)
{
	struct copy_state *state = opaque;
	uint64_t current_free;
	uint64_t copied = 0;
	unsigned int copy_percent = 0;
	int overall_percent;
	char status[256];
	if (!state)
		return;
	state->elapsed_seconds++;
	current_free = filesystem_free_bytes(".");
	if (state->initial_free_bytes > current_free)
		copied = state->initial_free_bytes - current_free;
	if (state->total_bytes) {
		uint64_t scaled = copied > state->total_bytes ? state->total_bytes : copied;
		copy_percent = (unsigned int)(scaled * 100ULL / state->total_bytes);
		if (copy_percent > 99U)
			copy_percent = 99U;
	}
	overall_percent = 42 + (int)(copy_percent * 28U / 100U);
	if (overall_percent > 69)
		overall_percent = 69;
	if (state->total_bytes)
		snprintf(status, sizeof(status),
			"Copying /usr to USB: %u%% (%" PRIu64 " / %" PRIu64 " MiB, %u s)",
			copy_percent, copied / (1024U * 1024U),
			state->total_bytes / (1024U * 1024U),
			state->elapsed_seconds);
	else
		snprintf(status, sizeof(status), "Copying /usr to USB... %u s elapsed",
			state->elapsed_seconds);
	if (state->callback)
		state->callback(overall_percent, status, state->opaque);
}

static void progress(storage_progress_cb callback, void *opaque, int percent,
	const char *status)
{
	if (callback)
		callback(percent, status, opaque);
}

struct prepare_state {
	const struct storage_device *device;
	storage_progress_cb callback;
	void *opaque;
	char *error;
	size_t error_size;
	int disk_fd;
	char part1[64];
	char part2[64];
	char data_uuid[128];
	char swap_uuid[128];
	int mounted;
	int mount_locked;
	int bound;
	int swap_active;
	int fstab_written;
};

struct partition_layout {
	uint64_t first_start;
	uint64_t first_size;
	uint64_t swap_start;
	uint64_t swap_sectors;
};

static int open_target_disk(struct prepare_state *state, uint64_t *bytes,
	unsigned int *sector_size)
{
	const struct storage_device *device = state->device;
	state->disk_fd = open(device->path, O_RDONLY | O_CLOEXEC);  /* NOSONAR the USB device the user chose */
	if (state->disk_fd < 0 || ioctl(state->disk_fd, BLKGETSIZE64, bytes) < 0) {
		set_error(state->error, state->error_size,
			"The size of %s cannot be read: %s", device->path, strerror(errno));
		return 0;
	}
	if (ioctl(state->disk_fd, BLKSSZGET, sector_size) < 0)
		*sector_size = 512;
	if (*bytes != device->size_bytes || *sector_size == 0 ||
		*bytes < SWAP_BYTES + MIN_DATA_BYTES) {
		set_error(state->error, state->error_size,
			"The USB device size has changed.");
		return 0;
	}
	return 1;
}

static int compute_layout(struct prepare_state *state, uint64_t bytes,
	unsigned int sector_size, struct partition_layout *layout)
{
	uint64_t total_sectors = bytes / sector_size;
	uint64_t alignment;
	if (total_sectors > 0xffffffffULL) {
		set_error(state->error, state->error_size,
			"USB devices larger than 2 TB are not supported by this version.");
		return 0;
	}
	alignment = 1024ULL * 1024ULL / sector_size;
	if (alignment == 0) alignment = 1;
	layout->first_start = alignment;
	layout->swap_sectors = SWAP_BYTES / sector_size;
	layout->swap_start = ((total_sectors - layout->swap_sectors) / alignment) *
		alignment;
	layout->first_size = layout->swap_start - layout->first_start;
	if (layout->first_size * sector_size < MIN_DATA_BYTES) {
		set_error(state->error, state->error_size, "The USB device is too small.");
		return 0;
	}
	return 1;
}

static int partition_disk(struct prepare_state *state)
{
	const struct storage_device *device = state->device;
	uint64_t bytes = 0;
	unsigned int sector_size = 512;
	struct partition_layout layout;
	char script[512];
	char sfdisk[PATH_MAX];
	char disk_path[sizeof(device->path)];
	char *argv[] = {sfdisk, "--wipe", "always", "--wipe-partitions",
		"always", disk_path, NULL};

	if (!open_target_disk(state, &bytes, &sector_size) ||
		!compute_layout(state, bytes, sector_size, &layout))
		return 0;
	partition_path(device, 1, state->part1, sizeof(state->part1));
	partition_path(device, 2, state->part2, sizeof(state->part2));
	snprintf(script, sizeof(script),
		"label: dos\nunit: sectors\n%s : start=%llu, size=%llu, type=83\n"
		"%s : start=%llu, size=%llu, type=82\n",
		state->part1, (unsigned long long)layout.first_start,
		(unsigned long long)layout.first_size, state->part2,
		(unsigned long long)layout.swap_start,
		(unsigned long long)layout.swap_sectors);
	if (!process_find("sfdisk", sfdisk, sizeof(sfdisk))) {
		set_error(state->error, state->error_size,
			"sfdisk is not installed in this image.");
		return 0;
	}
	progress(state->callback, state->opaque, 10,
		"Creating a new partition table...");
	snprintf(disk_path, sizeof(disk_path), "%s", device->path);
	if (process_run(argv, script, NULL, NULL) != 0) {
		set_error(state->error, state->error_size, "Partitioning %s failed.",
			device->path);
		return 0;
	}
	ioctl(state->disk_fd, BLKRRPART);
	close(state->disk_fd);
	state->disk_fd = -1;
	if (!wait_for_partitions(state->part1, state->part2)) {
		set_error(state->error, state->error_size,
			"The new partitions did not appear in the system.");
		return 0;
	}
	return 1;
}

static int format_partitions(struct prepare_state *state)
{
	char mkfs[PATH_MAX];
	char mkswap[PATH_MAX];
	char *mkfs_argv[] = {mkfs, "-F", "-t", "ext4", "-L",
		"FLASH_EXPANDER", "-m", "0", state->part1, NULL};
	char *mkswap_argv[] = {mkswap, "-L", "SMALLBOX_SWAP", state->part2, NULL};

	if (!process_find("mkfs.ext4", mkfs, sizeof(mkfs)) &&
		!process_find("mke2fs", mkfs, sizeof(mkfs))) {
		set_error(state->error, state->error_size,
			"mkfs.ext4 is not installed in this image.");
		return 0;
	}
	progress(state->callback, state->opaque, 22,
		"Formatting the FlashExpander as ext4...");
	if (process_run(mkfs_argv, NULL, NULL, NULL) != 0) {
		set_error(state->error, state->error_size,
			"Formatting the ext4 partition failed.");
		return 0;
	}
	if (!process_find("mkswap", mkswap, sizeof(mkswap))) {
		set_error(state->error, state->error_size,
			"mkswap is not installed in this image.");
		return 0;
	}
	progress(state->callback, state->opaque, 32,
		"Creating the 512 MB swap partition...");
	if (process_run(mkswap_argv, NULL, NULL, NULL) != 0) {
		set_error(state->error, state->error_size,
			"Formatting the swap partition failed.");
		return 0;
	}
	return 1;
}

static int mount_expander(struct prepare_state *state)
{
	char *mount_argv[] = {"mount", "-t", "ext4", "-o", "noatime",
		state->part1, EXPANDER_MOUNT, NULL};
	char *swapon_argv[] = {"swapon", state->part2, NULL};

	/* Drain any event which entered mount.sh just before the nomount flag. */
	sleep_ms(500);
	if (!deactivate_target(state->device, state->error, state->error_size))
		return 0;
	if (!make_directory(EXPANDER_MOUNT, 0755)) {
		set_error(state->error, state->error_size,
			"The mount directory cannot be created.");
		return 0;
	}
	if (process_run(mount_argv, NULL, NULL, NULL) != 0) {
		set_error(state->error, state->error_size,
			"The FlashExpander cannot be mounted.");
		return 0;
	}
	state->mounted = 1;
	/*
	 * Keep our current directory inside the filesystem while copying.  A
	 * traditional automounter can no longer unmount it, and relative paths
	 * remain on the USB filesystem even if an old lazy-unmount script races
	 * us.  This also prevents a copy from ever falling through into flash.
	 */
	if (chdir(EXPANDER_MOUNT) != 0) {
		set_error(state->error, state->error_size,
			"The FlashExpander mount cannot be locked: %s", strerror(errno));
		return 0;
	}
	state->mount_locked = 1;
	progress(state->callback, state->opaque, 38,
		"Activating 512 MB swap before copying /usr...");
	if (process_run(swapon_argv, NULL, NULL, NULL) != 0) {
		set_error(state->error, state->error_size,
			"Swap could not be activated.");
		return 0;
	}
	state->swap_active = 1;
	return 1;
}

static int copy_usr(struct prepare_state *state)
{
	struct copy_state copy;
	char *argv[] = {"cp", "-a", "/usr/.", EXPANDER_USR_REL, NULL};

	if (!make_directory(EXPANDER_USR_REL, 0755)) {
		set_error(state->error, state->error_size,
			"The destination for /usr cannot be created.");
		return 0;
	}
	progress(state->callback, state->opaque, 42,
		"Copying the complete /usr tree. Do not remove the USB device!");
	memset(&copy, 0, sizeof(copy));
	copy.callback = state->callback;
	copy.opaque = state->opaque;
	copy.total_bytes = directory_allocated_bytes("/usr");
	copy.initial_free_bytes = filesystem_free_bytes(".");
	if (process_run_with_updates(argv, NULL, copy_line, copy_tick, 1000,
		&copy) != 0) {
		if (copy.error_line[0])
			set_error(state->error, state->error_size,
				"Copying /usr failed: %s", copy.error_line);
		else
			set_error(state->error, state->error_size, "Copying /usr failed.");
		return 0;
	}
	sync();
	progress(state->callback, state->opaque, 70,
		"Copying /usr to USB: 100% complete.");
	if (access(EXPANDER_USR_REL "/bin", F_OK) != 0) {
		set_error(state->error, state->error_size,
			"The copied /usr tree is incomplete.");
		return 0;
	}
	return 1;
}

static int switch_usr(struct prepare_state *state)
{
	char *bind_argv[] = {"mount", "--bind", EXPANDER_USR, "/usr", NULL};
	struct stat source_status;
	struct stat target_status;

	progress(state->callback, state->opaque, 72,
		"Configuring UUIDs and persistent mounts...");
	if (!blkid_uuid(state->part1, state->data_uuid, sizeof(state->data_uuid)) ||
		!blkid_uuid(state->part2, state->swap_uuid, sizeof(state->swap_uuid))) {
		set_error(state->error, state->error_size,
			"The partition UUIDs cannot be read.");
		return 0;
	}
	if (!write_metadata(state->data_uuid, state->swap_uuid)) {
		set_error(state->error, state->error_size,
			"The FlashExpander metadata could not be saved.");
		return 0;
	}
	if (!update_fstab(state->data_uuid, state->swap_uuid, 1, state->error,
		state->error_size))
		return 0;
	state->fstab_written = 1;
	progress(state->callback, state->opaque, 82,
		"Switching /usr live to the USB device...");
	if (process_run(bind_argv, NULL, NULL, NULL) != 0) {
		set_error(state->error, state->error_size,
			"The bind mount for /usr failed.");
		return 0;
	}
	state->bound = 1;
	if (stat(EXPANDER_USR, &source_status) < 0 ||
		stat("/usr", &target_status) < 0 ||
		source_status.st_dev != target_status.st_dev) {
		set_error(state->error, state->error_size,
			"The /usr switch could not be verified.");
		return 0;
	}
	return 1;
}

static int release_working_directory(struct prepare_state *state)
{
	progress(state->callback, state->opaque, 92,
		"Verifying the active 512 MB swap...");
	sync();
	if (state->mount_locked) {
		if (chdir("/") < 0) {
			set_error(state->error, state->error_size,
				"The USB working directory cannot be released: %s",
				strerror(errno));
			return 0;
		}
		state->mount_locked = 0;
	}
	return 1;
}

static void prepare_cleanup(struct prepare_state *state)
{
	char *swapoff_argv[] = {"swapoff", state->part2, NULL};
	char *umount_usr_argv[] = {"umount", "/usr", NULL};
	char *umount_data_argv[] = {"umount", EXPANDER_MOUNT, NULL};

	if (state->disk_fd >= 0)
		close(state->disk_fd);
	if (state->fstab_written)
		update_fstab(NULL, NULL, 0, NULL, 0);
	if (state->swap_active)
		run_quiet(swapoff_argv);
	if (state->bound)
		run_quiet(umount_usr_argv);
	if (state->mount_locked && chdir("/") == 0)
		state->mount_locked = 0;
	if (state->mounted)
		run_quiet(umount_data_argv);
}

int storage_prepare(const struct storage_device *device,
	storage_progress_cb callback, void *opaque, char *uuid,
	size_t uuid_size, char *error, size_t error_size)
{
	struct prepare_state state;
	char nomount_flag[PATH_MAX];

	if (uuid && uuid_size) uuid[0] = '\0';
	if (error && error_size) error[0] = '\0';
	if (!device || !validate_selected(device)) {
		set_error(error, error_size,
			"The selected USB device is no longer safely available.");
		return 0;
	}
	/* This is the flag consumed by OpenATV's udev mount.sh. */
	if (!create_nomount_flag(device, nomount_flag, sizeof(nomount_flag))) {
		set_error(error, error_size,
			"The automatic mount guard cannot be created for %s.",
			device->path);
		return 0;
	}
	progress(callback, opaque, 3, "Preparing the USB device for exclusive use...");
	if (!deactivate_target(device, error, error_size))
		return 0;
	memset(&state, 0, sizeof(state));
	state.device = device;
	state.callback = callback;
	state.opaque = opaque;
	state.error = error;
	state.error_size = error_size;
	state.disk_fd = -1;
	if (!partition_disk(&state) || !format_partitions(&state) ||
		!mount_expander(&state) || !copy_usr(&state) ||
		!switch_usr(&state) || !release_working_directory(&state)) {
		prepare_cleanup(&state);
		return 0;
	}
	progress(callback, opaque, 100, "FlashExpander and swap are active.");
	if (uuid && uuid_size)
		snprintf(uuid, uuid_size, "%s", state.data_uuid);
	return 1;
}
