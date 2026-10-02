#define _GNU_SOURCE

#include "multiboot.h"
#include "process.h"
#include "version.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CONFIG_FILE "/etc/smallbox-wizard.conf"
#define STARTUP_MOUNT "/tmp/smallbox-startup"
#define WORK_DIRECTORY "smallbox"
#define SWAP_BYTES (512ULL * 1024ULL * 1024ULL)
#define MIN_ROOT_BYTES (512ULL * 1024ULL * 1024ULL)
#define MAX_FEED_BYTES (2U * 1024U * 1024U)
#define FSTAB_BEGIN "# SMALLBOX-CHKROOT BEGIN"
#define FSTAB_END "# SMALLBOX-CHKROOT END"

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

static void progress(multiboot_progress_cb callback, void *opaque, int percent,
	const char *status)
{
	if (callback)
		callback(percent, status, opaque);
}

static void trim(char *text)
{
	const char *start = text;
	size_t length;
	if (!text)
		return;
	while (*start && isspace((unsigned char)*start))  /* NOSONAR isspace on a char of a NUL-terminated string */
		start++;
	if (start != text)
		memmove(text, start, strlen(start) + 1);
	length = strlen(text);
	while (length && isspace((unsigned char)text[length - 1]))
		text[--length] = '\0';
}

static int ensure_directory(const char *path, mode_t mode)
{
	struct stat status;
	if (mkdir(path, mode) == 0)  /* NOSONAR a work or mount directory of the wizard */
		return 1;
	return errno == EEXIST && stat(path, &status) == 0 &&
		S_ISDIR(status.st_mode);
}

static int fsync_directory(const char *path)
{
	int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);  /* NOSONAR the directory of a file the wizard wrote */
	int result;
	if (fd < 0)
		return 0;
	result = fsync(fd) == 0;
	close(fd);
	return result;
}

static int write_atomic(const char *path, const char *text, mode_t mode)
{
	char temporary[PATH_MAX];
	char directory[PATH_MAX];
	char *slash;
	int fd;
	int written;
	FILE *file;
	if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path,
		(long)getpid()) >= (int)sizeof(temporary))
		return 0;
	fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);  /* NOSONAR next to a file of the wizard */
	if (fd < 0)
		return 0;
	file = fdopen(fd, "w");
	if (!file) {
		close(fd);
		unlink(temporary);  /* NOSONAR the temporary file created just above */
		return 0;
	}
	written = fputs(text, file) != EOF && fflush(file) == 0 && fsync(fd) == 0;
	if (fclose(file) != 0)
		written = 0;
	if (!written || rename(temporary, path) != 0) {  /* NOSONAR a file of the wizard */
		unlink(temporary);  /* NOSONAR the temporary file created above */
		return 0;
	}
	snprintf(directory, sizeof(directory), "%s", path);
	slash = strrchr(directory, '/');
	if (slash) {
		if (slash == directory)
			slash[1] = '\0';
		else
			*slash = '\0';
		fsync_directory(directory);
	}
	return 1;
}

static int read_text(const char *path, char *text, size_t text_size)
{
	FILE *file;
	size_t used;
	if (!text || text_size == 0)
		return 0;
	text[0] = '\0';
	file = fopen(path, "r");  /* NOSONAR /proc or a STARTUP file of the wizard */
	if (!file)
		return 0;
	used = fread(text, 1, text_size - 1, file);
	text[used] = '\0';  /* NOSONAR used is limited to the buffer just above */
	fclose(file);
	trim(text);
	return used != 0;
}

static void config_value(char *destination, size_t destination_size,
	const char *value)
{
	if (destination && destination_size)
		snprintf(destination, destination_size, "%s", value ? value : "");
}

static enum multiboot_policy parse_policy(const char *value)
{
	if (strcmp(value, "required") == 0)
		return MULTIBOOT_REQUIRED;
	if (strcmp(value, "optional") == 0)
		return MULTIBOOT_OPTIONAL;
	return MULTIBOOT_DISABLED;
}

static void apply_config_entry(struct multiboot_config *config, const char *key,
	const char *value)
{
	if (strcmp(key, "chkroot") == 0) {
		config->policy = parse_policy(value);
	} else if (strcmp(key, "machine") == 0) {
		config_value(config->machine, sizeof(config->machine), value);
	} else if (strcmp(key, "machine_build") == 0) {
		config_value(config->machine_build,
			sizeof(config->machine_build), value);
	} else if (strcmp(key, "mtd_kernel") == 0) {
		config_value(config->mtd_kernel, sizeof(config->mtd_kernel), value);
	} else if (strcmp(key, "image_feed") == 0) {
		config_value(config->image_feed, sizeof(config->image_feed), value);
	} else if (strcmp(key, "image_url") == 0) {
		config_value(config->image_url, sizeof(config->image_url), value);
	} else if (strcmp(key, "image_match") == 0) {
		config_value(config->image_match, sizeof(config->image_match), value);
	} else if (strcmp(key, "maximum_slots") == 0) {
		int slots = atoi(value);
		if (slots >= 1 && slots <= 16)
			config->maximum_slots = slots;
	} else if (strcmp(key, "single_core") == 0) {
		config->single_core = strcmp(value, "1") == 0 ||
			strcmp(value, "yes") == 0 || strcmp(value, "true") == 0;
	}
}

int multiboot_config_load(struct multiboot_config *config, char *error,
	size_t error_size)
{
	FILE *file;
	char line[1536];
	if (!config) {
		set_error(error, error_size, "The Multiboot configuration is invalid.");
		return 0;
	}
	memset(config, 0, sizeof(*config));
	config->policy = MULTIBOOT_DISABLED;
	config->maximum_slots = 4;
	config_value(config->image_match, sizeof(config->image_match),
		"_multiboot.zip");
	file = fopen(CONFIG_FILE, "r");
	if (!file) {
		set_error(error, error_size, "%s cannot be opened: %s", CONFIG_FILE,
			strerror(errno));
		return 0;
	}
	while (fgets(line, sizeof(line), file)) {
		char *separator;
		char *key;
		char *value;
		trim(line);
		if (!line[0] || line[0] == '#')
			continue;
		separator = strchr(line, '=');
		if (!separator)
			continue;
		*separator = '\0';
		key = line;
		value = separator + 1;
		trim(key);
		trim(value);
		apply_config_entry(config, key, value);
	}
	fclose(file);
	if (config->policy != MULTIBOOT_DISABLED &&
		(!config->machine_build[0] || !config->mtd_kernel[0])) {
		set_error(error, error_size,
			"The Multiboot machine or internal kernel device is missing.");
		return 0;
	}
	if (error && error_size)
		error[0] = '\0';
	return 1;
}

static int partition_path(const struct storage_device *device, int number,
	char *path, size_t path_size)
{
	size_t length;
	const char *separator;
	if (!device || !device->path[0])
		return 0;
	length = strlen(device->path);
	separator = length && isdigit((unsigned char)device->path[length - 1]) ?
		"p" : "";
	return snprintf(path, path_size, "%s%s%d", device->path, separator,
		number) < (int)path_size;
}

static int selected_device_is_valid(const struct storage_device *selected)
{
	struct storage_device devices[STORAGE_MAX_DEVICES];
	char error[128];
	int count;
	count = storage_scan_usb(devices, STORAGE_MAX_DEVICES, error,
		sizeof(error));
	for (int index = 0; index < count; ++index) {
		if (strcmp(devices[index].path, selected->path) == 0 &&
			devices[index].size_bytes == selected->size_bytes)
			return 1;
	}
	return 0;
}

static int source_is_partition(const char *source, const char *device)
{
	size_t length = strlen(device);
	if (strncmp(source, device, length) != 0)
		return 0;
	return source[length] == '\0' || source[length] == 'p' ||  /* NOSONAR prefix matched, index in bounds */
		isdigit((unsigned char)source[length]);
}

static void run_quiet(char *const argv[])
{
	process_run(argv, NULL, NULL, NULL);
}

static void swapoff_device(const struct storage_device *device)
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

static int deactivate_device(const struct storage_device *device, char *error,
	size_t error_size)
{
	FILE *file;
	char line[1024];
	char mounts[32][PATH_MAX];
	int mount_count = 0;
	swapoff_device(device);
	file = fopen("/proc/mounts", "r");
	if (!file)
		return 1;
	while (fgets(line, sizeof(line), file) && mount_count < 32) {
		char source[PATH_MAX];
		char mountpoint[PATH_MAX];
		if (sscanf(line, "%4095s %4095s", source, mountpoint) != 2 ||
			!source_is_partition(source, device->path))
			continue;
		if (strcmp(mountpoint, "/") == 0 || strcmp(mountpoint, "/usr") == 0 ||
			strcmp(mountpoint, "/boot") == 0) {
			fclose(file);
			set_error(error, error_size,
				"The selected device contains the running system (%s).",
				mountpoint);
			return 0;
		}
		snprintf(mounts[mount_count++], sizeof(mounts[0]), "%s", mountpoint);
	}
	fclose(file);
	for (int index = mount_count - 1; index >= 0; --index) {
		char *argv[] = {"umount", mounts[index], NULL};
		if (process_run(argv, NULL, NULL, NULL) != 0) {
			set_error(error, error_size, "Cannot unmount %s.", mounts[index]);
			return 0;
		}
	}
	return 1;
}

static int create_nomount(const char *name)
{
	char path[128];
	int fd;
	if (snprintf(path, sizeof(path), "/dev/nomount.%s", name) >=
		(int)sizeof(path))
		return 0;
	fd = open(path, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);  /* NOSONAR guard for a device the wizard found */
	if (fd < 0)
		return 0;
	close(fd);
	return 1;
}

static int wait_for_partitions(const char *part1, const char *part2,
	const char *part3)
{
	struct stat status1;
	struct stat status2;
	struct stat status3;
	const struct timespec delay = {0, 100000000L};
	for (int attempt = 0; attempt < 120; ++attempt) {
		if (stat(part1, &status1) == 0 && S_ISBLK(status1.st_mode) &&
			stat(part2, &status2) == 0 && S_ISBLK(status2.st_mode) &&
			stat(part3, &status3) == 0 && S_ISBLK(status3.st_mode))
			return 1;
		nanosleep(&delay, NULL);
	}
	return 0;
}

static int blkid_value(char *partition, const char *field, char *value,
	size_t value_size)
{
	char blkid[PATH_MAX];
	char field_argument[32];
	char *argv[] = {blkid, "-s", field_argument, "-o", "value", partition,
		NULL};
	snprintf(field_argument, sizeof(field_argument), "%s", field);
	if (!process_find("blkid", blkid, sizeof(blkid)) ||
		process_capture(argv, value, value_size) != 0)
		return 0;
	trim(value);
	return value[0] != '\0';
}

static int append_word(char *output, size_t output_size, const char *word)
{
	size_t used = strlen(output);
	size_t length = strlen(word);
	if (used + length + (used ? 1U : 0U) + 2U > output_size)
		return 0;
	if (used)
		output[used++] = ' ';
	memcpy(output + used, word, length);
	used += length;
	output[used] = '\0';
	return 1;
}

static int flash_boot_token(const char *token)
{
	return strncmp(token, "ubi.mtd=", 8) == 0 ||
		strncmp(token, "rootfstype=", 11) == 0 ||
		strncmp(token, "rootflags=", 10) == 0 ||
		strcmp(token, "rootwait") == 0 || strcmp(token, "rw") == 0 ||
		strcmp(token, "ro") == 0;
}

static int build_flash_startup(const struct multiboot_config *config,
	char *startup, size_t startup_size)
{
	char command_line[4096];
	char copy[4096];
	char kernel[96];
	char *saveptr = NULL;
	int root_found = 0;
	if (!read_text("/proc/cmdline", command_line, sizeof(command_line)))
		return 0;
	if (strncmp(config->mtd_kernel, "/dev/", 5) == 0)
		snprintf(kernel, sizeof(kernel), "kernel=%s", config->mtd_kernel);
	else
		snprintf(kernel, sizeof(kernel), "kernel=/dev/%s", config->mtd_kernel);
	startup[0] = '\0';
	if (!append_word(startup, startup_size, kernel))
		return 0;
	snprintf(copy, sizeof(copy), "%s", command_line);
	for (const char *token = strtok_r(copy, " \t\r\n", &saveptr); token;
		token = strtok_r(NULL, " \t\r\n", &saveptr)) {
		if (strncmp(token, "root=", 5) == 0) {
			root_found = 1;
			if (!append_word(startup, startup_size, token))
				return 0;
		} else if (flash_boot_token(token) &&
			!append_word(startup, startup_size, token)) {
			return 0;
		}
	}
	if (!root_found)
		return 0;
	if (strstr(startup, "ubi.mtd=") && !strstr(startup, "flash="))
		append_word(startup, startup_size, "flash=1");
	if (strlen(startup) + 2 > startup_size)
		return 0;
	strcat(startup, "\n");
	return 1;
}

static int write_startup_files(const struct multiboot_config *config,
	const struct multiboot_layout *layout, char *error, size_t error_size)
{
	char flash_startup[4096];
	char external_startup[1024];
	char kernel[96];
	char path[PATH_MAX];
	if (!ensure_directory(STARTUP_MOUNT, 0755)) {
		set_error(error, error_size, "The temporary STARTUP mount cannot be created.");
		return 0;
	}
	if (mount(layout->startup_partition, STARTUP_MOUNT, "vfat", 0, NULL) < 0) {
		set_error(error, error_size, "The STARTUP partition cannot be mounted: %s",
			strerror(errno));
		return 0;
	}
	if (!build_flash_startup(config, flash_startup, sizeof(flash_startup))) {
		set_error(error, error_size,
			"The internal flash boot parameters cannot be determined.");
		goto failed;
	}
	snprintf(path, sizeof(path), "%s/STARTUP_FLASH", STARTUP_MOUNT);
	if (!write_atomic(path, flash_startup, 0644))
		goto write_failed;
	snprintf(path, sizeof(path), "%s/STARTUP", STARTUP_MOUNT);
	if (!write_atomic(path, flash_startup, 0644))
		goto write_failed;
	if (strncmp(config->mtd_kernel, "/dev/", 5) == 0)
		snprintf(kernel, sizeof(kernel), "%s", config->mtd_kernel);
	else
		snprintf(kernel, sizeof(kernel), "/dev/%s", config->mtd_kernel);
	for (int slot = 1; slot <= layout->slot_count; ++slot) {
		snprintf(external_startup, sizeof(external_startup),
			"kernel=%s root=UUID=%s rootsubdir=linuxrootfs%d "
			"rootfstype=ext4\n", kernel, layout->root_uuid, slot);
		snprintf(path, sizeof(path), "%s/STARTUP_%d", STARTUP_MOUNT, slot);
		if (!write_atomic(path, external_startup, 0644))
			goto write_failed;
	}
	sync();
	if (umount(STARTUP_MOUNT) < 0) {
		set_error(error, error_size, "The STARTUP partition cannot be unmounted.");
		return 0;
	}
	return 1;

write_failed:
	set_error(error, error_size, "The STARTUP slot files cannot be written.");
failed:
	umount2(STARTUP_MOUNT, MNT_DETACH);
	return 0;
}

struct partition_tools {
	char sfdisk[PATH_MAX];
	char mkfs_vfat[PATH_MAX];
	char mkfs_ext4[PATH_MAX];
	char mkswap[PATH_MAX];
};

struct partition_plan {
	uint64_t startup_start;
	uint64_t startup_size;
	uint64_t root_start;
	uint64_t root_size;
	uint64_t swap_start;
	uint64_t swap_sectors;
};

struct prepare_state {
	int disk_fd;
	int root_mounted;
	int swap_active;
	multiboot_progress_cb callback;
	void *opaque;
};

static int init_layout(const struct storage_device *device,
	const struct multiboot_config *config, struct multiboot_layout *layout,
	char *error, size_t error_size)
{
	memset(layout, 0, sizeof(*layout));
	snprintf(layout->disk, sizeof(layout->disk), "%s", device->path);
	snprintf(layout->disk_name, sizeof(layout->disk_name), "%s", device->name);
	if (!partition_path(device, 1, layout->startup_partition,
		sizeof(layout->startup_partition)) ||
		!partition_path(device, 2, layout->root_partition,
		sizeof(layout->root_partition)) ||
		!partition_path(device, 3, layout->swap_partition,
		sizeof(layout->swap_partition))) {
		set_error(error, error_size, "The USB partition names are too long.");
		return 0;
	}
	layout->current_slot = 0;
	layout->target_slot = 1;
	layout->slot_count = (int)(device->size_bytes / (1024ULL * 1024ULL *
		1024ULL));
	if (layout->slot_count < 1)
		layout->slot_count = 1;
	if (layout->slot_count > config->maximum_slots)
		layout->slot_count = config->maximum_slots;
	if (snprintf(layout->media_mount, sizeof(layout->media_mount),
		"/media/%s", device->name) >= (int)sizeof(layout->media_mount)) {
		set_error(error, error_size, "The USB media mount path is too long.");
		return 0;
	}
	return 1;
}

static int plan_partitions(int disk_fd, uint64_t bytes, uint64_t device_bytes,
	struct partition_plan *plan, char *error, size_t error_size)
{
	unsigned int sector_size = 512;
	uint64_t total_sectors;
	uint64_t alignment;
	if (ioctl(disk_fd, BLKSSZGET, &sector_size) < 0 || sector_size == 0)
		sector_size = 512;
	if (bytes != device_bytes || bytes < SWAP_BYTES + MIN_ROOT_BYTES +
		8ULL * 1024ULL * 1024ULL) {
		set_error(error, error_size, "The USB device is too small or has changed.");
		return 0;
	}
	total_sectors = bytes / sector_size;
	alignment = 1024ULL * 1024ULL / sector_size;
	if (!alignment)
		alignment = 1;
	plan->startup_start = 4ULL * 1024ULL * 1024ULL / sector_size;
	plan->root_start = 5ULL * 1024ULL * 1024ULL / sector_size;
	if (plan->startup_start < alignment)
		plan->startup_start = alignment;
	if (plan->root_start <= plan->startup_start)
		plan->root_start = plan->startup_start + alignment;
	plan->swap_sectors = SWAP_BYTES / sector_size;
	if (total_sectors > plan->swap_sectors + plan->root_start + 34) {
		plan->swap_start = ((total_sectors - 34 - plan->swap_sectors) /
			alignment) * alignment;
		plan->root_size = plan->swap_start - plan->root_start;
		plan->startup_size = plan->root_start - plan->startup_start;
		if (plan->root_size * sector_size >= MIN_ROOT_BYTES && plan->startup_size)
			return 1;
	}
	set_error(error, error_size,
		"The USB device has insufficient space for rootfs and 512 MB swap.");
	return 0;
}

static int find_partition_tools(struct partition_tools *tools, char *error,
	size_t error_size)
{
	if (!process_find("sfdisk", tools->sfdisk, sizeof(tools->sfdisk)) ||
		!process_find("mkfs.vfat", tools->mkfs_vfat, sizeof(tools->mkfs_vfat)) ||
		(!process_find("mkfs.ext4", tools->mkfs_ext4, sizeof(tools->mkfs_ext4)) &&
		 !process_find("mke2fs", tools->mkfs_ext4, sizeof(tools->mkfs_ext4))) ||
		!process_find("mkswap", tools->mkswap, sizeof(tools->mkswap))) {
		set_error(error, error_size,
			"Required GPT, FAT, ext4 or swap tools are missing.");
		return 0;
	}
	return 1;
}

static int write_partition_table(struct partition_tools *tools,
	struct multiboot_layout *layout, const struct partition_plan *plan,
	char *error, size_t error_size)
{
	char script[2048];
	char *argv[] = {tools->sfdisk, "--wipe", "always", "--wipe-partitions",
		"always", layout->disk, NULL};
	snprintf(script, sizeof(script),
		"label: gpt\nunit: sectors\n"
		"%s : start=%llu, size=%llu, type=EBD0A0A2-B9E5-4433-87C0-68B6B72699C7, name=\"startup\"\n"
		"%s : start=%llu, size=%llu, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=\"rootfs\"\n"
		"%s : start=%llu, size=%llu, type=0657FD6D-A4AB-43C4-84E5-0933C84B4F4F, name=\"swap\"\n",
		layout->startup_partition, (unsigned long long)plan->startup_start,
		(unsigned long long)plan->startup_size, layout->root_partition,
		(unsigned long long)plan->root_start, (unsigned long long)plan->root_size,
		layout->swap_partition, (unsigned long long)plan->swap_start,
		(unsigned long long)plan->swap_sectors);
	if (process_run(argv, script, NULL, NULL) != 0) {
		set_error(error, error_size, "Creating the GPT partition table failed.");
		return 0;
	}
	return 1;
}

static void guard_new_partitions(const struct multiboot_layout *layout)
{
	char part_name[64];
	snprintf(part_name, sizeof(part_name), "%s",
		strrchr(layout->startup_partition, '/') + 1);
	create_nomount(part_name);
	snprintf(part_name, sizeof(part_name), "%s",
		strrchr(layout->root_partition, '/') + 1);
	create_nomount(part_name);
	snprintf(part_name, sizeof(part_name), "%s",
		strrchr(layout->swap_partition, '/') + 1);
	create_nomount(part_name);
}

static int format_partitions(struct partition_tools *tools,
	struct multiboot_layout *layout, multiboot_progress_cb callback,
	void *opaque, char *error, size_t error_size)
{
	progress(callback, opaque, 20, "Formatting the STARTUP partition as FAT32...");
	{
		char *argv[] = {tools->mkfs_vfat, "-F", "32", "-n", "STARTUP",
			layout->startup_partition, NULL};
		if (process_run(argv, NULL, NULL, NULL) != 0) {
			set_error(error, error_size, "Formatting the STARTUP partition failed.");
			return 0;
		}
	}
	progress(callback, opaque, 30,
		"Formatting the Chkroot rootfs partition as legacy-compatible ext4...");
	{
		char *argv[] = {tools->mkfs_ext4, "-F", "-t", "ext4", "-L", "rootfs",
			"-m", "0", "-O",
			"^64bit,^extent,^flex_bg,^huge_file,^dir_nlink,^extra_isize,^metadata_csum",
			layout->root_partition, NULL};
		if (process_run(argv, NULL, NULL, NULL) != 0) {
			set_error(error, error_size, "Formatting the rootfs partition failed.");
			return 0;
		}
	}
	progress(callback, opaque, 42, "Creating and activating exactly 512 MB swap...");
	{
		char *argv[] = {tools->mkswap, "-L", "swap", layout->swap_partition, NULL};
		if (process_run(argv, NULL, NULL, NULL) != 0) {
			set_error(error, error_size, "Formatting the swap partition failed.");
			return 0;
		}
	}
	return 1;
}

static int activate_swap(struct multiboot_layout *layout, char *error,
	size_t error_size)
{
	char *argv[] = {"swapon", layout->swap_partition, NULL};
	if (process_run(argv, NULL, NULL, NULL) != 0) {
		set_error(error, error_size, "Activating the 512 MB swap failed.");
		return 0;
	}
	return 1;
}

static int read_partition_uuids(struct multiboot_layout *layout, char *error,
	size_t error_size)
{
	if (!blkid_value(layout->root_partition, "UUID", layout->root_uuid,
		sizeof(layout->root_uuid)) ||
		!blkid_value(layout->swap_partition, "UUID", layout->swap_uuid,
		sizeof(layout->swap_uuid))) {
		set_error(error, error_size, "The rootfs or swap UUID cannot be read.");
		return 0;
	}
	return 1;
}

static int mount_media_root(const struct multiboot_layout *layout, char *error,
	size_t error_size)
{
	if (!ensure_directory("/media", 0755) ||
		!ensure_directory(layout->media_mount, 0755)) {
		set_error(error, error_size, "The persistent media mount cannot be created.");
		return 0;
	}
	if (mount(layout->root_partition, layout->media_mount, "ext4", 0, NULL) < 0) {
		set_error(error, error_size, "The Chkroot rootfs partition cannot be mounted.");
		return 0;
	}
	return 1;
}

static int write_layout_metadata(const struct multiboot_layout *layout,
	char *error, size_t error_size)
{
	char metadata[512];
	char metadata_path[PATH_MAX];
	snprintf(metadata, sizeof(metadata),
		"version=1\nmode=chkroot\ndisk=%s\nroot_uuid=%s\nswap_uuid=%s\n"
		"target_slot=%d\nslots=%d\n", layout->disk, layout->root_uuid,
		layout->swap_uuid, layout->target_slot, layout->slot_count);
	snprintf(metadata_path, sizeof(metadata_path), "%s/.smallbox-multiboot",
		layout->media_mount);
	if (!write_atomic(metadata_path, metadata, 0644)) {
		set_error(error, error_size, "The Multiboot metadata cannot be saved.");
		return 0;
	}
	return 1;
}

static int prepare_device(const struct storage_device *device,
	const struct multiboot_config *config, struct multiboot_layout *layout,
	struct prepare_state *state, char *error, size_t error_size)
{
	struct partition_tools tools;
	struct partition_plan plan;
	uint64_t bytes;
	if (!init_layout(device, config, layout, error, error_size))
		return 0;
	progress(state->callback, state->opaque, 3,
		"Locking the selected USB device against automatic mounts...");
	if (!create_nomount(device->name)) {
		set_error(error, error_size, "The automatic mount guard cannot be created.");
		return 0;
	}
	if (!deactivate_device(device, error, error_size))
		return 0;
	state->disk_fd = open(device->path, O_RDONLY | O_CLOEXEC);  /* NOSONAR the USB device the user chose */
	if (state->disk_fd < 0 || ioctl(state->disk_fd, BLKGETSIZE64, &bytes) < 0) {
		set_error(error, error_size, "The USB device size cannot be read: %s",
			strerror(errno));
		return 0;
	}
	if (!plan_partitions(state->disk_fd, bytes, device->size_bytes, &plan,
		error, error_size) || !find_partition_tools(&tools, error, error_size))
		return 0;
	progress(state->callback, state->opaque, 10,
		"Creating the STARTUP, rootfs and 512 MB swap partitions...");
	if (!write_partition_table(&tools, layout, &plan, error, error_size))
		return 0;
	ioctl(state->disk_fd, BLKRRPART);
	close(state->disk_fd);
	state->disk_fd = -1;
	guard_new_partitions(layout);
	if (!wait_for_partitions(layout->startup_partition, layout->root_partition,
		layout->swap_partition)) {
		set_error(error, error_size, "The new USB partitions did not appear.");
		return 0;
	}
	if (!format_partitions(&tools, layout, state->callback, state->opaque, error, error_size) ||
		!activate_swap(layout, error, error_size))
		return 0;
	state->swap_active = 1;
	if (!read_partition_uuids(layout, error, error_size))
		return 0;
	progress(state->callback, state->opaque, 50, "Writing and verifying the Multiboot slots...");
	if (!write_startup_files(config, layout, error, error_size) ||
		!mount_media_root(layout, error, error_size))
		return 0;
	state->root_mounted = 1;
	if (!write_layout_metadata(layout, error, error_size))
		return 0;
	progress(state->callback, state->opaque, 100,
		"Chkroot partitions, swap and STARTUP slots are ready.");
	return 1;
}

static void release_prepare(struct multiboot_layout *layout,
	const struct prepare_state *state)
{
	if (state->disk_fd >= 0)
		close(state->disk_fd);
	if (state->root_mounted)
		umount2(layout->media_mount, MNT_DETACH);
	if (state->swap_active) {
		char *argv[] = {"swapoff", layout->swap_partition, NULL};
		run_quiet(argv);
	}
}

int multiboot_prepare(const struct storage_device *device,
	const struct multiboot_config *config, multiboot_progress_cb callback,
	void *opaque, struct multiboot_layout *layout, char *error,
	size_t error_size)
{
	struct prepare_state state = {.disk_fd = -1, .callback = callback, .opaque = opaque};
	if (error && error_size)
		error[0] = '\0';
	if (!device || !config || !layout || !selected_device_is_valid(device)) {
		set_error(error, error_size,
			"The selected USB device is no longer safely available.");
		return 0;
	}
	if (prepare_device(device, config, layout, &state, error, error_size))
		return 1;
	release_prepare(layout, &state);
	return 0;
}

static char *read_allocated_file(const char *path, size_t maximum,
	size_t *length, char *error, size_t error_size)
{
	FILE *file;
	long size;
	char *data;
	file = fopen(path, "rb");  /* NOSONAR the image list the wizard downloaded */
	if (!file) {
		set_error(error, error_size, "Cannot read %s: %s", path,
			strerror(errno));
		return NULL;
	}
	size = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
	if (size < 0 || (size_t)size > maximum || fseek(file, 0, SEEK_SET) != 0) {
		fclose(file);
		set_error(error, error_size, "The downloaded image list is invalid.");
		return NULL;
	}
	data = malloc((size_t)size + 1);
	if (!data) {
		fclose(file);
		set_error(error, error_size, "Not enough memory to read the image list.");
		return NULL;
	}
	if (fread(data, 1, (size_t)size, file) != (size_t)size) {
		free(data);
		fclose(file);
		set_error(error, error_size, "The image list could not be read completely.");
		return NULL;
	}
	data[size] = '\0';
	fclose(file);
	if (length)
		*length = (size_t)size;
	return data;
}

static const char *skip_json_space(const char *cursor)
{
	while (*cursor && isspace((unsigned char)*cursor))
		cursor++;
	return cursor;
}

static int decode_json_string(const char *cursor, char *output,
	size_t output_size, const char **after)
{
	size_t used = 0;
	if (*cursor != '"')
		return 0;
	cursor++;
	while (*cursor && *cursor != '"') {
		char value = *cursor++;
		if (value == '\\') {
			value = *cursor++;
			if (!value)
				return 0;
			if (value == 'n') value = '\n';
			else if (value == 'r') value = '\r';
			else if (value == 't') value = '\t';
			else if (value == 'u')
				return 0;
		}
		if (used + 1 >= output_size)
			return 0;
		output[used++] = value;
	}
	if (*cursor != '"')
		return 0;
	output[used] = '\0';
	if (after)
		*after = cursor + 1;
	return 1;
}

static unsigned long long json_number_in_object(const char *object,
	const char *object_end, const char *key)
{
	const char *found = strstr(object, key);
	char *end;
	unsigned long long value;
	if (!found || found >= object_end)
		return 0;
	found = strchr(found, ':');
	if (!found || found >= object_end)
		return 0;
	found = skip_json_space(found + 1);
	errno = 0;
	value = strtoull(found, &end, 10);
	return errno == 0 && end != found ? value : 0;
}

static int select_feed_image(const char *json_path,
	const struct multiboot_config *config, char *url, size_t url_size,
	uint64_t *expected_size, char *error, size_t error_size)
{
	char *json;
	const char *cursor;
	unsigned long long best_date = 0;
	uint64_t best_size = 0;
	char best_url[1024] = "";
	json = read_allocated_file(json_path, MAX_FEED_BYTES, NULL, error,
		error_size);
	if (!json)
		return 0;
	cursor = json;
	while ((cursor = strstr(cursor, "\"link\"")) != NULL) {
		char candidate[1024];
		const char *object_end = strchr(cursor, '}');
		const char *value = strchr(cursor, ':');
		const char *after;
		unsigned long long date;
		uint64_t size;
		if (!object_end || !value)
			break;
		value = skip_json_space(value + 1);
		if (!decode_json_string(value, candidate, sizeof(candidate), &after)) {
			cursor += 6;
			continue;
		}
		(void)after;
		if ((config->image_match[0] &&
			!strstr(candidate, config->image_match)) ||
			(config->machine_build[0] &&
			 !strstr(candidate, config->machine_build))) {
			cursor = object_end + 1;
			continue;
		}
		date = json_number_in_object(cursor, object_end, "\"date\"");
		size = json_number_in_object(cursor, object_end, "\"size\"");
		if (!best_url[0] || date >= best_date) {
			snprintf(best_url, sizeof(best_url), "%s", candidate);
			best_date = date;
			best_size = size;
		}
		cursor = object_end + 1;
	}
	free(json);
	if (!best_url[0]) {
		set_error(error, error_size,
			"No matching SmallBox Multiboot image was found in the image feed.");
		return 0;
	}
	snprintf(url, url_size, "%s", best_url);
	if (expected_size)
		*expected_size = best_size;
	return 1;
}

struct transfer_state {
	multiboot_progress_cb callback;
	void *opaque;
	char path[PATH_MAX];
	uint64_t expected;
	time_t started;
	char last_line[192];
};

static void transfer_line(const char *line, void *opaque)
{
	struct transfer_state *state = opaque;
	if (state && line)
		snprintf(state->last_line, sizeof(state->last_line), "%s", line);
}

static void transfer_tick(void *opaque)
{
	struct transfer_state *state = opaque;
	struct stat status;
	uint64_t received = 0;
	int percent = 12;
	char detail[256];
	long elapsed;
	if (!state)
		return;
	if (stat(state->path, &status) == 0 && status.st_size > 0)
		received = (uint64_t)status.st_size;
	if (state->expected) {
		uint64_t scaled = received > state->expected ? state->expected : received;
		percent = 12 + (int)(scaled * 38ULL / state->expected);
	}
	elapsed = (long)(time(NULL) - state->started);
	if (elapsed < 0)
		elapsed = 0;
	if (state->expected)
		snprintf(detail, sizeof(detail),
			"Downloading full image: %llu / %llu MiB (%d%%), %ld s",
			received / 1024ULL / 1024ULL,
			state->expected / 1024ULL / 1024ULL,
			(int)(received * 100ULL / state->expected), elapsed);
	else
		snprintf(detail, sizeof(detail),
			"Downloading full image: %llu MiB, %ld s - %.100s",
			received / 1024ULL / 1024ULL, elapsed,
			state->last_line);
	progress(state->callback, state->opaque, percent, detail);
}

static int file_status(const char *path, struct stat *status)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);  /* NOSONAR the download of the wizard */
	int result;
	if (fd < 0)
		return -1;
	result = fstat(fd, status);
	close(fd);
	return result;
}

static int download_file(char *url, const char *path, uint64_t expected,
	multiboot_progress_cb callback, void *opaque, char *error,
	size_t error_size)
{
	char wget[PATH_MAX];
	char temporary[PATH_MAX];
	struct transfer_state state;
	struct stat status;
	int result;
	if (!process_find("wget", wget, sizeof(wget))) {
		set_error(error, error_size, "wget is not installed in the bootstrap image.");
		return 0;
	}
	if (snprintf(temporary, sizeof(temporary), "%s.part", path) >=
		(int)sizeof(temporary)) {
		set_error(error, error_size, "The image download path is too long.");
		return 0;
	}
	unlink(temporary);  /* NOSONAR a leftover partial download of the wizard */
	memset(&state, 0, sizeof(state));
	state.callback = callback;
	state.opaque = opaque;
	state.expected = expected;
	state.started = time(NULL);
	snprintf(state.path, sizeof(state.path), "%s", temporary);
	progress(callback, opaque, 12, "Starting the full-image download...");
	{
		char *argv[] = {wget, "-O", temporary, url, NULL};
		result = process_run_with_updates(argv, NULL, transfer_line,
			transfer_tick, 1000, &state);
	}
	if (result != 0 || file_status(temporary, &status) != 0 || status.st_size <= 0) {
		set_error(error, error_size, "Downloading the full image failed%s%s.",
			state.last_line[0] ? ": " : "",
			state.last_line[0] ? state.last_line : "");
		unlink(temporary);  /* NOSONAR the partial download of the wizard */
		return 0;
	}
	if (expected && (uint64_t)status.st_size != expected) {
		set_error(error, error_size,
			"The downloaded image size is invalid (%llu instead of %llu bytes).",
			(unsigned long long)status.st_size,
			(unsigned long long)expected);
		unlink(temporary);  /* NOSONAR the partial download of the wizard */
		return 0;
	}
	if (rename(temporary, path) != 0) {  /* NOSONAR the download of the wizard */
		set_error(error, error_size, "The downloaded image cannot be finalized.");
		unlink(temporary);  /* NOSONAR the partial download of the wizard */
		return 0;
	}
	return 1;
}

struct timed_state {
	multiboot_progress_cb callback;
	void *opaque;
	int percent;
	time_t started;
	char action[128];
	char line[128];
	char history[512];
	char issues[384];
	FILE *log;
};

static void remember_text(char *buffer, size_t buffer_size, const char *line,
	size_t maximum_line)
{
	const char *kept = line;
	size_t length;
	size_t used;
	const char *newline;
	if (!buffer || buffer_size < 2 || !line)
		return;
	length = strlen(kept);
	if (length > maximum_line) {
		kept += length - maximum_line;
		length = maximum_line;
	}
	used = strlen(buffer);
	while (used && used + length + 2 >= buffer_size) {
		newline = strchr(buffer, '\n');
		if (!newline) {
			buffer[0] = '\0';
			used = 0;
			break;
		}
		memmove(buffer, newline + 1, strlen(newline + 1) + 1);
		used = strlen(buffer);
	}
	if (used && used + 1 < buffer_size)
		buffer[used++] = '\n';
	if (used < buffer_size - 1) {
		length = length < buffer_size - used - 1 ? length :
			buffer_size - used - 1;
		memcpy(buffer + used, kept, length);
		buffer[used + length] = '\0';
	}
}

static int diagnostic_line(const char *line)
{
	return line && (strstr(line, "Error") || strstr(line, "error") ||
		strstr(line, "Failed") || strstr(line, "failed") ||
		strstr(line, "Cannot") || strstr(line, "cannot") ||
		strstr(line, "can't") || strstr(line, "Abort") ||
		strstr(line, "will not"));
}

static void remember_timed_line(struct timed_state *state, const char *line)
{
	if (!state || !line)
		return;
	remember_text(state->history, sizeof(state->history), line, 220);
	if (diagnostic_line(line))
		remember_text(state->issues, sizeof(state->issues), line, 180);
}

static void timed_line(const char *line, void *opaque)
{
	struct timed_state *state = opaque;
	if (!state || !line)
		return;
	snprintf(state->line, sizeof(state->line), "%s", line);
	remember_timed_line(state, line);
	if (state->log) {
		fprintf(state->log, "%s\n", line);
		fflush(state->log);
	}
}

static void timed_tick(void *opaque)
{
	struct timed_state *state = opaque;
	char detail[320];
	long elapsed;
	if (!state)
		return;
	elapsed = (long)(time(NULL) - state->started);
	if (elapsed < 0)
		elapsed = 0;
	snprintf(detail, sizeof(detail), "%s - %ld s - %.120s", state->action,
		elapsed, state->line[0] ? state->line : "Please wait...");
	progress(state->callback, state->opaque, state->percent, detail);
}

static int rootfs_filename(const char *name)
{
	size_t length = strlen(name);
	return strcmp(name, "rootfs.bin") == 0 ||
		strcmp(name, "root_cfe_auto.bin") == 0 ||
		strcmp(name, "root_cfe_auto.jffs2") == 0 ||
		strcmp(name, "oe_rootfs.bin") == 0 ||
		strcmp(name, "e2jffs2.img") == 0 ||
		strcmp(name, "rootfs.ubi") == 0 ||
		strcmp(name, "rootfs.tar.bz2") == 0 ||
		strcmp(name, "rootfs-one.tar.bz2") == 0 ||
		strcmp(name, "rootfs-two.tar.bz2") == 0 ||
		(length > 4 && strcmp(name + length - 4, ".nfi") == 0);
}

static int find_image_directory(const char *directory, int depth, char *result,
	size_t result_size)
{
	DIR *stream;
	struct dirent *entry;
	char child[PATH_MAX];
	struct stat status;
	if (depth < 0)
		return 0;
	stream = opendir(directory);  /* NOSONAR inside the image the wizard unpacked */
	if (!stream)
		return 0;
	while ((entry = readdir(stream)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;
		if (rootfs_filename(entry->d_name)) {
			snprintf(result, result_size, "%s", directory);
			closedir(stream);
			return 1;
		}
	}
	rewinddir(stream);
	while ((entry = readdir(stream)) != NULL) {
		if (entry->d_name[0] == '.')
			continue;
		if (snprintf(child, sizeof(child), "%s/%s", directory,
			entry->d_name) >= (int)sizeof(child) || lstat(child, &status) != 0 ||
			!S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode))
			continue;
		if (find_image_directory(child, depth - 1, result, result_size)) {
			closedir(stream);
			return 1;
		}
	}
	closedir(stream);
	return 0;
}

static int update_target_fstab(const struct multiboot_layout *layout,
	const char *root_directory, char *error, size_t error_size)
{
	char path[PATH_MAX];
	char temporary[PATH_MAX];
	FILE *input;
	FILE *output;
	char line[1024];
	int skip = 0;
	int fd;
	int written;
	if (snprintf(path, sizeof(path), "%s/etc/fstab", root_directory) >=
		(int)sizeof(path) || snprintf(temporary, sizeof(temporary),
		"%s/etc/fstab.smallbox.XXXXXX", root_directory) >=
		(int)sizeof(temporary)) {
		set_error(error, error_size, "The installed fstab path is too long.");
		return 0;
	}
	input = fopen(path, "r");  /* NOSONAR fstab in the slot the wizard installed */
	fd = mkstemp(temporary);
	if (fd < 0) {
		if (input) fclose(input);
		set_error(error, error_size, "The installed fstab cannot be prepared.");
		return 0;
	}
	fchmod(fd, 0644);
	output = fdopen(fd, "w");
	if (!output) {
		close(fd);
		unlink(temporary);  /* NOSONAR the temporary file created just above */
		if (input) fclose(input);
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
	fprintf(output, "\n%s\n", FSTAB_BEGIN);
	fprintf(output, "UUID=%s %s ext4 defaults,noatime 0 0\n",
		layout->root_uuid, layout->media_mount);
	fprintf(output, "UUID=%s none swap sw 0 0\n", layout->swap_uuid);
	fprintf(output, "%s\n", FSTAB_END);
	written = fflush(output) == 0 && fsync(fd) == 0;
	if (fclose(output) != 0)
		written = 0;
	if (!written || rename(temporary, path) != 0) {  /* NOSONAR fstab in the slot the wizard installed */
		unlink(temporary);  /* NOSONAR the temporary file created above */
		set_error(error, error_size, "The installed fstab cannot be saved.");
		return 0;
	}
	return 1;
}

static int configure_installed_root(const struct multiboot_config *config,
	const struct multiboot_layout *layout, char *error, size_t error_size)
{
	char root_directory[PATH_MAX];
	char media_directory[PATH_MAX];
	char path[PATH_MAX];
	char marker[512];
	if (snprintf(root_directory, sizeof(root_directory), "%s/linuxrootfs%d",
		layout->media_mount, layout->target_slot) >= (int)sizeof(root_directory))
		return 0;
	if (snprintf(path, sizeof(path), "%s/usr/bin/enigma2", root_directory) >=
		(int)sizeof(path)) {
		set_error(error, error_size, "The installed Enigma2 path is too long.");
		return 0;
	}
	if (access(path, X_OK) != 0) {
		set_error(error, error_size,
			"The installed slot is incomplete: /usr/bin/enigma2 is missing.");
		return 0;
	}
	if (snprintf(path, sizeof(path), "%s/sbin/init", root_directory) >=
		(int)sizeof(path)) {
		set_error(error, error_size, "The installed init path is too long.");
		return 0;
	}
	if (access(path, F_OK) != 0) {
		set_error(error, error_size,
			"The installed slot is incomplete: /sbin/init is missing.");
		return 0;
	}
	if (snprintf(media_directory, sizeof(media_directory), "%s/media",
		root_directory) >= (int)sizeof(media_directory) ||
		!ensure_directory(media_directory, 0755)) {
		set_error(error, error_size,
			"The installed /media directory cannot be created.");
		return 0;
	}
	if (snprintf(path, sizeof(path), "%s%s", root_directory,
		layout->media_mount) >= (int)sizeof(path)) {
		set_error(error, error_size,
			"The installed USB media mount path is too long.");
		return 0;
	}
	if (!ensure_directory(path, 0755)) {
		set_error(error, error_size,
			"The installed /media mount directory cannot be created.");
		return 0;
	}
	if (!update_target_fstab(layout, root_directory, error, error_size))
		return 0;
	snprintf(marker, sizeof(marker),
		"version=%s\nmode=chkroot\nmachine=%s\nslot=%d\n"
		"root_uuid=%s\nswap_uuid=%s\n", SMALLBOX_WIZARD_VERSION,
		config->machine_build,
		layout->target_slot, layout->root_uuid, layout->swap_uuid);
	if (snprintf(path, sizeof(path), "%s/etc/smallbox-wizard.done",
		root_directory) >= (int)sizeof(path)) {
		set_error(error, error_size,
			"The installed completion-marker path is too long.");
		return 0;
	}
	if (!write_atomic(path, marker, 0644)) {
		set_error(error, error_size,
			"The completion marker cannot be written into the installed slot.");
		return 0;
	}
	sync();
	return 1;
}

static void normalize_startup(char *text)
{
	const char *read_cursor = text;
	char *write_cursor = text;
	int spacing = 0;
	while (*read_cursor) {
		if (isspace((unsigned char)*read_cursor)) {  /* NOSONAR isspace on a char of a NUL-terminated string */
			spacing = write_cursor != text;
		} else {
			if (spacing)
				*write_cursor++ = ' ';
			*write_cursor++ = *read_cursor;
			spacing = 0;
		}
		read_cursor++;
	}
	*write_cursor = '\0';
}

int multiboot_detect_active_slot(const struct multiboot_layout *layout,
	int *slot, char *error, size_t error_size)
{
	char active[4096];
	char candidate[4096];
	char path[PATH_MAX];
	if (slot)
		*slot = -1;
	if (!layout || !ensure_directory(STARTUP_MOUNT, 0755) ||
		mount(layout->startup_partition, STARTUP_MOUNT, "vfat", 0, NULL) < 0) {
		set_error(error, error_size, "The STARTUP partition cannot be checked.");
		return 0;
	}
	snprintf(path, sizeof(path), "%s/STARTUP", STARTUP_MOUNT);
	if (!read_text(path, active, sizeof(active)))
		goto not_found;
	normalize_startup(active);
	snprintf(path, sizeof(path), "%s/STARTUP_FLASH", STARTUP_MOUNT);
	if (read_text(path, candidate, sizeof(candidate))) {
		normalize_startup(candidate);
		if (strcmp(active, candidate) == 0) {
			if (slot) *slot = 0;
			umount(STARTUP_MOUNT);
			return 1;
		}
	}
	for (int index = 1; index <= layout->slot_count; ++index) {
		snprintf(path, sizeof(path), "%s/STARTUP_%d", STARTUP_MOUNT, index);
		if (!read_text(path, candidate, sizeof(candidate)))
			continue;
		normalize_startup(candidate);
		if (strcmp(active, candidate) == 0) {
			if (slot) *slot = index;
			umount(STARTUP_MOUNT);
			return 1;
		}
	}

not_found:
	umount2(STARTUP_MOUNT, MNT_DETACH);
	set_error(error, error_size,
		"The active STARTUP file does not match a known Multiboot slot.");
	return 0;
}

static int activate_slot(const struct multiboot_layout *layout, char *error,
	size_t error_size)
{
	char source_path[PATH_MAX];
	char target_path[PATH_MAX];
	char startup[4096];
	int detected;
	if (!ensure_directory(STARTUP_MOUNT, 0755) ||
		mount(layout->startup_partition, STARTUP_MOUNT, "vfat", 0, NULL) < 0) {
		set_error(error, error_size, "The STARTUP partition cannot be activated.");
		return 0;
	}
	snprintf(source_path, sizeof(source_path), "%s/STARTUP_%d", STARTUP_MOUNT,
		layout->target_slot);
	snprintf(target_path, sizeof(target_path), "%s/STARTUP", STARTUP_MOUNT);
	if (!read_text(source_path, startup, sizeof(startup)) ||
		!strstr(startup, layout->root_uuid)) {
		set_error(error, error_size,
			"The target STARTUP slot does not contain the expected rootfs UUID.");
		umount2(STARTUP_MOUNT, MNT_DETACH);
		return 0;
	}
	if (strlen(startup) + 2 > sizeof(startup)) {
		set_error(error, error_size, "The target STARTUP file is too long.");
		umount2(STARTUP_MOUNT, MNT_DETACH);
		return 0;
	}
	strcat(startup, "\n");
	if (!write_atomic(target_path, startup, 0644)) {
		set_error(error, error_size, "The target slot cannot be made active.");
		umount2(STARTUP_MOUNT, MNT_DETACH);
		return 0;
	}
	sync();
	if (umount(STARTUP_MOUNT) < 0) {
		set_error(error, error_size, "The activated STARTUP file cannot be flushed.");
		return 0;
	}
	if (!multiboot_detect_active_slot(layout, &detected, error, error_size) ||
		detected != layout->target_slot) {
		if (!error || !error[0])
			set_error(error, error_size, "The wrong Multiboot slot became active.");
		return 0;
	}
	return 1;
}

struct install_paths {
	char work[PATH_MAX];
	char feed[PATH_MAX];
	char archive[PATH_MAX];
	char unpack[PATH_MAX];
	char image_directory[PATH_MAX];
	char url[1024];
	uint64_t expected_size;
};

static int prepare_install_paths(const struct multiboot_layout *layout,
	struct install_paths *paths, char *error, size_t error_size)
{
	if (snprintf(paths->work, sizeof(paths->work), "%s/%s", layout->media_mount,
		WORK_DIRECTORY) >= (int)sizeof(paths->work) ||
		snprintf(paths->feed, sizeof(paths->feed), "%s/images.json",
			paths->work) >= (int)sizeof(paths->feed) ||
		snprintf(paths->archive, sizeof(paths->archive),
			"%s/smallbox-multiboot.zip", paths->work) >= (int)sizeof(paths->archive) ||
		snprintf(paths->unpack, sizeof(paths->unpack), "%s/unpacked-%ld",
			paths->work, (long)getpid()) >= (int)sizeof(paths->unpack)) {
		set_error(error, error_size, "The external image path is too long.");
		return 0;
	}
	if (!ensure_directory(paths->work, 0755) ||
		!ensure_directory(paths->unpack, 0755)) {
		set_error(error, error_size, "The external image directory cannot be created.");
		return 0;
	}
	return 1;
}

/* The image to download into paths->url, from the configuration or the feed. */
static int resolve_image_url(const struct multiboot_config *config,
	struct install_paths *paths, multiboot_progress_cb callback, void *opaque,
	char *error, size_t error_size)
{
	char wget[PATH_MAX];
	char feed_url[sizeof(config->image_feed)];
	if (config->image_url[0]) {
		snprintf(paths->url, sizeof(paths->url), "%s", config->image_url);
		return 1;
	}
	if (!config->image_feed[0] || !process_find("wget", wget, sizeof(wget))) {
		set_error(error, error_size, "No SmallBox Multiboot image source is configured.");
		return 0;
	}
	progress(callback, opaque, 5, "Loading the OE-Alliance image list...");
	snprintf(feed_url, sizeof(feed_url), "%s", config->image_feed);
	{
		char *argv[] = {wget, "-O", paths->feed, feed_url, NULL};
		if (process_run(argv, NULL, NULL, NULL) != 0) {
			set_error(error, error_size, "The OE-Alliance image list cannot be downloaded.");
			return 0;
		}
	}
	return select_feed_image(paths->feed, config, paths->url, sizeof(paths->url),
		&paths->expected_size, error, error_size);
}

static void init_timed_state(struct timed_state *timed,
	multiboot_progress_cb callback, void *opaque, int percent)
{
	memset(timed, 0, sizeof(*timed));
	timed->callback = callback;
	timed->opaque = opaque;
	timed->percent = percent;
	timed->started = time(NULL);
}

static int unpack_image(struct install_paths *paths,
	multiboot_progress_cb callback, void *opaque, char *error,
	size_t error_size)
{
	char unzip[PATH_MAX];
	struct timed_state timed;
	int result;
	if (!process_find("unzip", unzip, sizeof(unzip))) {
		set_error(error, error_size, "unzip is not installed in the bootstrap image.");
		return 0;
	}
	init_timed_state(&timed, callback, opaque, 52);
	snprintf(timed.action, sizeof(timed.action), "Extracting the full image");
	progress(callback, opaque, 52, "Extracting the full image on USB...");
	{
		char *argv[] = {unzip, "-o", paths->archive, "-d", paths->unpack, NULL};
		result = process_run_with_updates(argv, NULL, timed_line, timed_tick,
			1000, &timed);
	}
	if (result != 0 || !find_image_directory(paths->unpack, 5,
		paths->image_directory, sizeof(paths->image_directory))) {
		set_error(error, error_size,
			"The downloaded archive does not contain a usable rootfs image.");
		return 0;
	}
	return 1;
}

static int run_ofgwrite(const struct multiboot_layout *layout,
	struct install_paths *paths, multiboot_progress_cb callback,
	multiboot_display_clear_cb clear_display, void *opaque, char *error,
	size_t error_size)
{
	char ofgwrite[PATH_MAX];
	char root_argument[96];
	char current_argument[32];
	char target_argument[32];
	char ofgwrite_log[PATH_MAX];
	struct timed_state timed;
	const char *diagnostic;
	int result;
	if (!process_find("ofgwrite", ofgwrite, sizeof(ofgwrite))) {
		set_error(error, error_size, "ofgwrite is not installed in the bootstrap image.");
		return 0;
	}
	snprintf(root_argument, sizeof(root_argument), "-r%s",
		layout->root_partition + 5);
	snprintf(current_argument, sizeof(current_argument), "-c%d",
		layout->current_slot);
	snprintf(target_argument, sizeof(target_argument), "-m%d",
		layout->target_slot);
	if (snprintf(ofgwrite_log, sizeof(ofgwrite_log), "%s/ofgwrite.log",
		paths->work) >= (int)sizeof(ofgwrite_log)) {
		set_error(error, error_size, "The ofgwrite log path is too long.");
		return 0;
	}
	/* Rootfs only: a -k option must never be added here.  One internal kernel
	 * is shared by all Chkroot slots. */
	progress(callback, opaque, 62,
		"Starting ofgwrite for the selected Chkroot slot. The display will switch to ofgwrite...");
	init_timed_state(&timed, callback, opaque, 70);
	snprintf(timed.action, sizeof(timed.action),
		"Installing rootfs-only into USB slot %d", layout->target_slot);
	timed.log = fopen(ofgwrite_log, "w");  /* NOSONAR the log in the work directory of the wizard */
	if (timed.log) {
		fprintf(timed.log,
			"Command: %s -q %s %s %s %s\n",
			ofgwrite, root_argument, current_argument, target_argument,
			paths->image_directory);
		fflush(timed.log);
	}
	if (clear_display)
		clear_display(opaque);
	{
		char *argv[] = {ofgwrite, "-q", root_argument, current_argument,
			target_argument, paths->image_directory, NULL};
		/* ofgwrite owns the framebuffer until it exits.  Capture its text output,
		 * but do not let the SmallBox progress callback draw over its UI. */
		result = process_run(argv, NULL, timed_line, &timed);
	}
	if (timed.log) {
		fprintf(timed.log, "Exit status: %d\n", result);
		fclose(timed.log);
		timed.log = NULL;
	}
	progress(callback, opaque, result == 0 ? 88 : 70,
		result == 0 ?
		"ofgwrite completed. Restoring the SmallBox Wizard display..." :
		"ofgwrite returned an error. Restoring the diagnostic screen...");
	if (result != 0) {
		diagnostic = timed.issues[0] ? timed.issues : timed.history;
		if (!diagnostic[0])
			diagnostic = "No text output was captured.";
		set_error(error, error_size,
			"ofgwrite failed while installing rootfs-only into slot %d (status %d).\n"
			"Diagnostic output:\n%.300s\nComplete log on USB: smallbox/ofgwrite.log",
			layout->target_slot, result, diagnostic);
		return 0;
	}
	return 1;
}

int multiboot_install(const struct multiboot_config *config,
	const struct multiboot_layout *layout, multiboot_progress_cb callback,
	multiboot_display_clear_cb clear_display, void *opaque, char *error,
	size_t error_size)
{
	struct install_paths paths = {.expected_size = 0};
	int active_slot;
	if (error && error_size)
		error[0] = '\0';
	if (!config || !layout || layout->target_slot <= 0 ||
		strncmp(layout->root_partition, "/dev/", 5) != 0) {
		set_error(error, error_size, "The Chkroot installation target is invalid.");
		return 0;
	}
	if (!multiboot_detect_active_slot(layout, &active_slot, error, error_size) ||
		active_slot != layout->current_slot) {
		set_error(error, error_size,
			"Installation stopped because the active slot is not internal flash.");
		return 0;
	}
	if (!prepare_install_paths(layout, &paths, error, error_size) ||
		!resolve_image_url(config, &paths, callback, opaque, error, error_size) ||
		!download_file(paths.url, paths.archive, paths.expected_size, callback,
		opaque, error, error_size) ||
		!unpack_image(&paths, callback, opaque, error, error_size) ||
		!run_ofgwrite(layout, &paths, callback, clear_display, opaque, error,
		error_size))
		return 0;
	progress(callback, opaque, 90,
		"Verifying the installed Enigma2 rootfs and persistent mounts...");
	if (!configure_installed_root(config, layout, error, error_size))
		return 0;
	progress(callback, opaque, 96,
		"Activating the verified USB slot for the next boot...");
	if (!activate_slot(layout, error, error_size))
		return 0;
	progress(callback, opaque, 100,
		"The verified Chkroot slot is active. The internal kernel was unchanged.");
	return 1;
}
