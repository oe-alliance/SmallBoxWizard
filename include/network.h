#ifndef SMALLBOX_NETWORK_H
#define SMALLBOX_NETWORK_H

#include <stddef.h>

#define NETWORK_MAX_INTERFACES 12
#define NETWORK_PACKAGE_NAME_SIZE 128
#define NETWORK_PACKAGE_DETAIL_SIZE 384

struct network_interface {
	char name[32];
	char address[48];
	int wireless;
	int link;
};

typedef void (*network_progress_cb)(int percent, const char *status,
	void *opaque);

struct network_package_failure {
	char package[NETWORK_PACKAGE_NAME_SIZE];
	char detail[NETWORK_PACKAGE_DETAIL_SIZE];
	int status;
	int total;
	int installed;
	int remaining;
	int skipped;
};

enum network_package_action {
	NETWORK_PACKAGE_ABORT = 0,
	NETWORK_PACKAGE_SKIP,
	NETWORK_PACKAGE_RETRY
};

typedef enum network_package_action (*network_package_failure_cb)(
	const struct network_package_failure *failure, void *opaque);

int network_scan(struct network_interface interfaces[], int maximum);
int network_configure_dhcp(const char *interface,
	network_progress_cb callback, void *opaque,
	char *address, size_t address_size, char *error, size_t error_size);
int network_has_ipv4(char *interface, size_t interface_size,
	char *address, size_t address_size);
int network_synchronize_time(network_progress_cb callback, void *opaque,
	char *error, size_t error_size);
int network_install_smallbox(network_progress_cb callback,
	network_package_failure_cb failure_callback, void *opaque,
	char *error, size_t error_size);
int network_disable_optional_services(network_progress_cb callback,
	void *opaque, char *error, size_t error_size);

#endif
