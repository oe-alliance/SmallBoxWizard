#ifndef SMALLBOX_MULTIBOOT_H
#define SMALLBOX_MULTIBOOT_H

#include "storage.h"

#include <stddef.h>
#include <stdint.h>

#define MULTIBOOT_PATH_SIZE 512

enum multiboot_policy {
	MULTIBOOT_DISABLED = 0,
	MULTIBOOT_OPTIONAL,
	MULTIBOOT_REQUIRED
};

struct multiboot_config {
	enum multiboot_policy policy;
	char machine[64];
	char machine_build[64];
	char mtd_kernel[64];
	char image_feed[512];
	char image_url[1024];
	char image_match[128];
	int maximum_slots;
	int single_core;
};

struct multiboot_layout {
	char disk[64];
	char disk_name[32];
	char startup_partition[64];
	char root_partition[64];
	char swap_partition[64];
	char root_uuid[128];
	char swap_uuid[128];
	char media_mount[MULTIBOOT_PATH_SIZE];
	int current_slot;
	int target_slot;
	int slot_count;
};

typedef void (*multiboot_progress_cb)(int percent, const char *status,
	void *opaque);
typedef void (*multiboot_display_clear_cb)(void *opaque);

int multiboot_config_load(struct multiboot_config *config, char *error,
	size_t error_size);
int multiboot_prepare(const struct storage_device *device,
	const struct multiboot_config *config, multiboot_progress_cb callback,
	void *opaque, struct multiboot_layout *layout, char *error,
	size_t error_size);
int multiboot_install(const struct multiboot_config *config,
	struct multiboot_layout *layout, multiboot_progress_cb callback,
	multiboot_display_clear_cb clear_display, void *opaque, char *error,
	size_t error_size);
int multiboot_detect_active_slot(const struct multiboot_layout *layout,
	int *slot, char *error, size_t error_size);

#endif
