#ifndef SMALLBOX_STORAGE_H
#define SMALLBOX_STORAGE_H

#include <stddef.h>
#include <stdint.h>

#define STORAGE_MAX_DEVICES 12

struct storage_device {
	char name[32];
	char path[64];
	char model[96];
	uint64_t size_bytes;
	int removable;
};

typedef void (*storage_progress_cb)(int percent, const char *status,
	void *opaque);

int storage_scan_usb(struct storage_device devices[], int maximum,
	char *error, size_t error_size);
int storage_is_expander_active(char *uuid, size_t uuid_size);
int storage_reset_expander(char *error, size_t error_size);
int storage_prepare(const struct storage_device *device,
	storage_progress_cb callback, void *opaque, char *uuid,
	size_t uuid_size, char *error, size_t error_size);
void storage_format_size(uint64_t bytes, char *text, size_t text_size);
const char *storage_expander_mount(void);

#endif
