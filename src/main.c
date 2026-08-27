#define _GNU_SOURCE

#include "input.h"
#include "multiboot.h"
#include "network.h"
#include "process.h"
#include "storage.h"
#include "ui.h"
#include "version.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define VERSION SMALLBOX_WIZARD_VERSION
#define DONE_MARKER "/etc/smallbox-wizard.done"
#define REBOOT_MARKER "/tmp/smallbox-wizard-rebooting"

struct application {
	struct ui_context ui;
	struct input_context input;
	char detail[512];
	int no_reboot;
};

static volatile sig_atomic_t stop_requested;

static void signal_handler(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static void install_signal_handlers(void)
{
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = signal_handler;
	sigemptyset(&action.sa_mask);
	sigaction(SIGINT, &action, NULL);
	sigaction(SIGTERM, &action, NULL);
	sigaction(SIGHUP, &action, NULL);
}

static enum input_key wait_key(struct application *app)
{
	enum input_key key;
	while (!stop_requested) {
		key = input_wait(&app->input, 1000);
		if (key != INPUT_NONE)
			return key;
		if (app->input.count == 0) {
			input_close(&app->input);
			input_open(&app->input);
		}
	}
	return INPUT_BACK;
}

static void wait_acknowledge(struct application *app)
{
	enum input_key key;
	do {
		key = wait_key(app);
	} while (key != INPUT_OK && key != INPUT_BACK && key != INPUT_RED &&
		!stop_requested);
}

static int choose_menu(struct application *app, const char *title,
	const char *body, const char *const items[], int count, int selected,
	const char *footer)
{
	enum input_key key;
	if (count <= 0)
		return -1;
	if (selected < 0 || selected >= count)
		selected = 0;
	for (;;) {
		ui_menu(&app->ui, title, body, items, count, selected, footer);
		key = wait_key(app);
		if (stop_requested || key == INPUT_BACK || key == INPUT_RED)
			return -1;
		if (key == INPUT_UP || key == INPUT_LEFT) {
			selected = (selected + count - 1) % count;
		} else if (key == INPUT_DOWN || key == INPUT_RIGHT) {
			selected = (selected + 1) % count;
		} else if (key == INPUT_OK || key == INPUT_GREEN) {
			return selected;
		}
	}
}

static void show_error(struct application *app, const char *title,
	const char *message)
{
	ui_error(&app->ui, title, message);
	wait_acknowledge(app);
}

static void discard_pending_input(struct application *app)
{
	if (!app)
		return;
	while (input_wait(&app->input, 0) != INPUT_NONE)
		;
}

static void storage_progress(int percent, const char *status, void *opaque)
{
	struct application *app = opaque;
	discard_pending_input(app);
	snprintf(app->detail, sizeof(app->detail), "%s", status ? status : "");
	ui_progress(&app->ui, "SET UP USB STORAGE",
		"WARNING: DO NOT REMOVE THE USB DEVICE OR POWER OFF THE RECEIVER!",
		percent, app->detail, "PLEASE WAIT...");
}

static void network_progress(int percent, const char *status, void *opaque)
{
	struct application *app = opaque;
	discard_pending_input(app);
	snprintf(app->detail, sizeof(app->detail), "%s", status ? status : "");
	ui_progress(&app->ui, "SMALLBOX PREPARATION",
		"CONFIGURING THE NETWORK AND PACKAGES.", percent,
		app->detail, "PLEASE WAIT...");
}

static enum network_package_action package_failure(
	const struct network_package_failure *failure, void *opaque)
{
	struct application *app = opaque;
	const char *items[3];
	char body[1024];
	char skip_item[192];
	int count = 0;
	int skip_index = -1;
	int retry_index;
	int abort_index;
	int choice;

	if (!app || !failure)
		return NETWORK_PACKAGE_ABORT;
	discard_pending_input(app);
	if (failure->package[0]) {
		snprintf(skip_item, sizeof(skip_item),
			"SKIP %.120s AND CONTINUE", failure->package);
		skip_index = count;
		items[count++] = skip_item;
	}
	retry_index = count;
	items[count++] = "RETRY THE REMAINING PACKAGES";
	abort_index = count;
	items[count++] = "ABORT PACKAGE INSTALLATION";
	snprintf(body, sizeof(body),
		"OPKG STOPPED WITH STATUS %d.\n\n"
		"PACKAGE: %s\n"
		"INSTALLED: %d / %d    REMAINING: %d    SKIPPED: %d\n\n"
		"%.380s\n\nFULL LOG: /.FlashExpander/.smallbox-opkg/install.log",
		failure->status,
		failure->package[0] ? failure->package : "NOT DETECTED - SEE LOG",
		failure->installed, failure->total, failure->remaining,
		failure->skipped,
		failure->detail[0] ? failure->detail : "No detailed error line was reported.");
	choice = choose_menu(app, "PACKAGE INSTALLATION PROBLEM", body,
		items, count, 0, "ARROWS: SELECT   OK: CONTINUE   RED: ABORT");
	if (choice == skip_index)
		return NETWORK_PACKAGE_SKIP;
	if (choice == retry_index)
		return NETWORK_PACKAGE_RETRY;
	(void)abort_index;
	return NETWORK_PACKAGE_ABORT;
}

static void multiboot_progress(int percent, const char *status, void *opaque)
{
	struct application *app = opaque;
	discard_pending_input(app);
	snprintf(app->detail, sizeof(app->detail), "%s", status ? status : "");
	ui_progress(&app->ui, "CHKROOT MULTIBOOT",
		"WARNING: DO NOT REMOVE THE USB DEVICE OR POWER OFF THE RECEIVER!",
		percent, app->detail, "PLEASE WAIT...");
}

static void multiboot_clear_display(void *opaque)
{
	struct application *app = opaque;
	struct ui_color black = {0, 0, 0, 255};
	if (!app)
		return;
	ui_clear(&app->ui, black);
	ui_present(&app->ui);
}

static int confirm_erase(struct application *app,
	const struct storage_device *device, int chkroot)
{
	char body[768];
	char size[32];
	const char *first[] = {
		"NO - GO BACK",
		"YES - ERASE ALL DATA"
	};
	const char *second[] = {
		"CANCEL",
		"PARTITION NOW"
	};
	int choice;
	storage_format_size(device->size_bytes, size, sizeof(size));
	if (chkroot)
		snprintf(body, sizeof(body),
			"WARNING: ALL DATA ON %s (%s, %s) WILL BE PERMANENTLY ERASED. "
			"A FAT32 STARTUP PARTITION, AN EXT4 CHKROOT PARTITION AND EXACTLY "
			"512 MB SWAP WILL BE CREATED.", device->path, device->model, size);
	else
		snprintf(body, sizeof(body),
			"WARNING: ALL DATA ON %s (%s, %s) WILL BE PERMANENTLY ERASED. "
			"AN EXT4 PARTITION FOR /USR AND A 512 MB SWAP PARTITION WILL BE CREATED.",
			device->path, device->model, size);
	choice = choose_menu(app, "CONFIRM DATA LOSS", body, first, 2, 0,
		"ARROWS: SELECT   OK: CONFIRM   RED: BACK");
	if (choice != 1)
		return 0;
	snprintf(body, sizeof(body),
		"FINAL SAFETY CHECK FOR %s. ONCE STARTED, THE USB DEVICE MUST NOT BE "
		"REMOVED. A POWER FAILURE MAY LEAVE THE RECEIVER UNUSABLE UNTIL IT IS "
		"FLASHED AGAIN.", device->path);
	choice = choose_menu(app, "FINAL CONFIRMATION", body, second, 2, 0,
		"CANCEL IS THE DEFAULT");
	return choice == 1;
}

static int configure_storage(struct application *app, char *uuid,
	size_t uuid_size)
{
	struct storage_device devices[STORAGE_MAX_DEVICES];
	char labels[UI_MAX_ITEMS][160];
	const char *items[UI_MAX_ITEMS];
	char error[512];
	int count;
	int i;
	int choice;
	for (;;) {
		count = storage_scan_usb(devices, STORAGE_MAX_DEVICES, error,
			sizeof(error));
		if (count < 0) {
			show_error(app, "USB DETECTION", error);
			return 0;
		}
		for (i = 0; i < count && i < UI_MAX_ITEMS - 1; ++i) {
			char size[32];
			storage_format_size(devices[i].size_bytes, size, sizeof(size));
			snprintf(labels[i], sizeof(labels[i]), "%s - %s - %s",
				devices[i].path, devices[i].model, size);
			items[i] = labels[i];
		}
		count = i;
		items[count] = "SCAN FOR USB DEVICES AGAIN";
		choice = choose_menu(app, "SELECT A USB DEVICE",
			count ?
			"SELECT THE USB DEVICE FOR THE FLASHEXPANDER. ONLY REAL USB BLOCK DEVICES ARE SHOWN." :
			"CONNECT A USB DEVICE WITH AT LEAST 1 GB, THEN SCAN AGAIN.",
			items, count + 1, 0,
			"ARROWS: SELECT   OK: CONTINUE");
		if (stop_requested)
			return 0;
		if (choice < 0 || choice == count)
			continue;
		if (!confirm_erase(app, &devices[choice], 0))
			continue;
		if (storage_prepare(&devices[choice], storage_progress, app, uuid,
			uuid_size, error, sizeof(error)))
			return 1;
		show_error(app, "USB SETUP FAILED", error);
	}
}

static int configure_multiboot_storage(struct application *app,
	const struct multiboot_config *config, struct multiboot_layout *layout)
{
	struct storage_device devices[STORAGE_MAX_DEVICES];
	char labels[UI_MAX_ITEMS][160];
	const char *items[UI_MAX_ITEMS];
	char error[512];
	int count;
	int choice;
	int index;
	for (;;) {
		count = storage_scan_usb(devices, STORAGE_MAX_DEVICES, error,
			sizeof(error));
		if (count < 0) {
			show_error(app, "USB DETECTION", error);
			return 0;
		}
		for (index = 0; index < count && index < UI_MAX_ITEMS - 1; ++index) {
			char size[32];
			storage_format_size(devices[index].size_bytes, size, sizeof(size));
			snprintf(labels[index], sizeof(labels[index]), "%s - %s - %s",
				devices[index].path, devices[index].model, size);
			items[index] = labels[index];
		}
		count = index;
		items[count] = "SCAN FOR USB DEVICES AGAIN";
		choice = choose_menu(app, "SELECT A CHKROOT DEVICE",
			count ?
			"SELECT THE USB DEVICE THAT WILL HOLD THE COMPLETE ENIGMA2 ROOTFS. ONLY REAL USB BLOCK DEVICES ARE SHOWN." :
			"CONNECT A USB DEVICE WITH AT LEAST 2 GB, THEN SCAN AGAIN.",
			items, count + 1, 0, "ARROWS: SELECT   OK: CONTINUE");
		if (stop_requested)
			return 0;
		if (choice < 0 || choice == count)
			continue;
		if (!confirm_erase(app, &devices[choice], 1))
			continue;
		if (multiboot_prepare(&devices[choice], config, multiboot_progress,
			app, layout, error, sizeof(error)))
			return 1;
		show_error(app, "CHKROOT USB SETUP FAILED", error);
	}
}

static int configure_network(struct application *app, char *chosen_interface,
	size_t interface_size, char *chosen_address, size_t address_size)
{
	struct network_interface interfaces[NETWORK_MAX_INTERFACES];
	char labels[UI_MAX_ITEMS][160];
	const char *items[UI_MAX_ITEMS];
	char current_interface[32] = "";
	char current_address[48] = "";
	char error[512];
	int count;
	int item_count;
	int map[UI_MAX_ITEMS];
	int i;
	int choice;
	for (;;) {
		count = network_scan(interfaces, NETWORK_MAX_INTERFACES);
		item_count = 0;
		if (network_has_ipv4(current_interface, sizeof(current_interface),
			current_address, sizeof(current_address))) {
			snprintf(labels[item_count], sizeof(labels[item_count]),
				"EXISTING CONNECTION: %.31s - IP %.47s", current_interface,
				current_address);
			items[item_count] = labels[item_count];
			map[item_count++] = -2;
		}
		for (i = 0; i < count && item_count < UI_MAX_ITEMS - 1; ++i) {
			if (interfaces[i].wireless)
				continue;
			snprintf(labels[item_count], sizeof(labels[item_count]),
				"LAN %.31s - %s", interfaces[i].name,
				interfaces[i].link ? "CABLE CONNECTED" : "NO LINK");
			items[item_count] = labels[item_count];
			map[item_count++] = i;
		}
		items[item_count] = "SCAN FOR NETWORK INTERFACES AGAIN";
		map[item_count++] = -3;
		choice = choose_menu(app, "SET UP THE NETWORK",
			"THE SMALLBOX INSTALLATION REQUIRES INTERNET ACCESS. THIS FIRST VERSION CONFIGURES WIRED LAN VIA DHCP; WI-FI CAN BE CONFIGURED LATER IN ENIGMA2.",
			items, item_count, 0,
			"ARROWS: SELECT   OK: CONNECT");
		if (stop_requested)
			return 0;
		if (choice < 0 || map[choice] == -3)
			continue;
		if (map[choice] == -2) {
			snprintf(chosen_interface, interface_size, "%s", current_interface);
			snprintf(chosen_address, address_size, "%s", current_address);
			return 1;
		}
		i = map[choice];
		if (network_configure_dhcp(interfaces[i].name, network_progress, app,
			chosen_address, address_size, error, sizeof(error))) {
			snprintf(chosen_interface, interface_size, "%s", interfaces[i].name);
			return 1;
		}
		show_error(app, "NETWORK SETUP FAILED", error);
	}
}

static int install_multiboot_image(struct application *app,
	const struct multiboot_config *config, struct multiboot_layout *layout)
{
	const char *items[] = {
		"DOWNLOAD AND INSTALL THE FULL IMAGE",
		"SET UP THE NETWORK AGAIN"
	};
	char error[512];
	int choice;
	for (;;) {
		choice = choose_menu(app, "INSTALL CHKROOT IMAGE",
			"THE WIZARD WILL DOWNLOAD THE SERVER-PINNED SMALLBOX MULTIBOOT IMAGE, VERIFY IT AND INSTALL ROOTFS-ONLY INTO USB SLOT 1. THE INTERNAL KERNEL WILL NOT BE FLASHED.",
			items, 2, 0, "ARROWS: SELECT   OK: START");
		if (stop_requested)
			return 0;
		if (choice < 0)
			continue;
		if (choice == 1)
			return -1;
		if (multiboot_install(config, layout, multiboot_progress,
			multiboot_clear_display, app, error, sizeof(error)))
			return 1;
		show_error(app, "CHKROOT INSTALLATION FAILED", error);
	}
}

static int install_packages(struct application *app)
{
	const char *items[] = {
		"INSTALL THE SMALLBOX PACKAGES NOW",
		"SET UP THE NETWORK AGAIN"
	};
	char error[512];
	int choice;
	for (;;) {
		choice = choose_menu(app, "INSTALL PACKAGES",
			"OPKG WILL DOWNLOAD THE SMALLBOX SOFTWARE AND ALL DEPENDENCIES. /USR IS ALREADY ON THE FLASHEXPANDER.",
			items, 2, 0,
			"ARROWS: SELECT   OK: START");
		if (stop_requested)
			return 0;
		if (choice < 0)
			continue;
		if (choice == 1)
			return -1;
		if (network_install_smallbox(network_progress, package_failure, app, error,
			sizeof(error)))
			return 1;
		show_error(app, "PACKAGE INSTALLATION FAILED", error);
	}
}

static int write_done_marker(const char *uuid, const char *interface,
	const char *address, char *error, size_t error_size)
{
	char temporary[] = "/etc/smallbox-wizard.done.XXXXXX";
	int fd = mkstemp(temporary);
	FILE *file;
	time_t now = time(NULL);
	if (fd < 0) {
		snprintf(error, error_size, "The completion marker cannot be created: %s",
			strerror(errno));
		return 0;
	}
	fchmod(fd, 0644);
	file = fdopen(fd, "w");
	if (!file) {
		close(fd);
		unlink(temporary);
		snprintf(error, error_size, "The completion marker cannot be written.");
		return 0;
	}
	fprintf(file, "version=%s\ncompleted=%lld\nuuid=%s\ninterface=%s\nip=%s\n",
		VERSION, (long long)now, uuid ? uuid : "", interface ? interface : "",
		address ? address : "");
	if (fflush(file) != 0 || fsync(fd) != 0 || fclose(file) != 0 ||
		rename(temporary, DONE_MARKER) != 0) {
		unlink(temporary);
		snprintf(error, error_size, "The completion marker could not be saved: %s",
			strerror(errno));
		return 0;
	}
	return 1;
}

static int eth0_ipv4(char *address, size_t address_size)
{
	struct network_interface interfaces[NETWORK_MAX_INTERFACES];
	int count;
	int i;
	if (!address || address_size == 0)
		return 0;
	address[0] = '\0';
	count = network_scan(interfaces, NETWORK_MAX_INTERFACES);
	for (i = 0; i < count; ++i) {
		if (strcmp(interfaces[i].name, "eth0") == 0 &&
			interfaces[i].address[0]) {
			snprintf(address, address_size, "%s", interfaces[i].address);
			return 1;
		}
	}
	return 0;
}

static void write_reboot_marker(void)
{
	int fd = open(REBOOT_MARKER, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
		0600);
	if (fd >= 0) {
		if (write(fd, "chkroot\n", 8) == 8)
			fsync(fd);
		close(fd);
	}
}

static int run_wizard(struct application *app)
{
	struct multiboot_config multiboot_config;
	struct multiboot_layout multiboot_layout;
	char uuid[128] = "";
	char interface[32] = "";
	char address[48] = "";
	char welcome_title[96];
	char welcome_body[768];
	char mode_body[1024];
	char eth0_address[48] = "";
	char error[512];
	int active;
	int choice;
	int chkroot = 0;
	const char *optional_modes[] = {
		"FLASHEXPANDER - /USR ON USB",
		"CHKROOT MULTIBOOT - FULL ROOTFS ON USB"
	};
	const char *required_mode[] = {
		"SET UP REQUIRED CHKROOT MULTIBOOT"
	};
	const char *recovery_modes[] = {
		"CONTINUE PREVIOUS SETUP",
		"START OVER WITH USB SETUP"
	};

	if (!multiboot_config_load(&multiboot_config, error, sizeof(error))) {
		show_error(app, "WIZARD CONFIGURATION", error);
		return 1;
	}

	snprintf(welcome_title, sizeof(welcome_title),
		"OE-ALLIANCE SMALLBOX WIZARD %s", VERSION);
	active = storage_is_expander_active(uuid, sizeof(uuid));
	if (active && multiboot_config.policy != MULTIBOOT_REQUIRED) {
		for (;;) {
			choice = choose_menu(app, "PREVIOUS SETUP FOUND",
				"THE PREVIOUS SETUP STOPPED AFTER USB STORAGE WAS PREPARED.\n\n"
				"CONTINUE KEEPS THE ACTIVE /USR AND SWAP.\n"
				"START OVER DISCONNECTS THEM AND RETURNS TO USB SELECTION.\n\n"
				"NO DATA IS ERASED UNTIL YOU CONFIRM IT AGAIN.",
				recovery_modes, 2, 0,
				"ARROWS: SELECT   OK: CONTINUE");
			if (stop_requested)
				return 2;
			if (choice < 0)
				continue;
			if (choice == 0)
				break;
			ui_progress(&app->ui, "RESET SMALLBOX SETUP",
				"DISCONNECTING THE PREVIOUS USB SETUP SAFELY.", 50,
				"NO DATA IS BEING ERASED.", "PLEASE WAIT...");
			if (storage_reset_expander(error, sizeof(error))) {
				active = 0;
				uuid[0] = '\0';
				break;
			}
			show_error(app, "RESET FAILED", error);
		}
	}
	if (!active || multiboot_config.policy == MULTIBOOT_REQUIRED) {
		if (eth0_ipv4(eth0_address, sizeof(eth0_address)))
			snprintf(welcome_body, sizeof(welcome_body),
				"THIS WIZARD MUST BE COMPLETED BEFORE ENIGMA2 CAN START.\n\n"
				"IT PREPARES THIS RECEIVER FOR LIMITED FLASH AND RAM.\n\n"
				"BOX IP (ETH0): %s\n\n"
				"PRESS OK TO START SETUP.",
				eth0_address);
		else
			snprintf(welcome_body, sizeof(welcome_body),
				"THIS WIZARD MUST BE COMPLETED BEFORE ENIGMA2 CAN START.\n\n"
				"IT PREPARES THIS RECEIVER FOR LIMITED FLASH AND RAM.\n\n"
				"BOX IP (ETH0): NOT AVAILABLE\n\n"
				"PRESS OK TO START SETUP.");
		ui_screen(&app->ui, welcome_title, welcome_body,
			"OK: START SETUP");
		wait_acknowledge(app);
		if (stop_requested)
			return 2;
		if (multiboot_config.policy == MULTIBOOT_OPTIONAL) {
			snprintf(mode_body, sizeof(mode_body),
				"CHOOSE HOW THIS RECEIVER WILL USE USB STORAGE.\n\n"
				"FLASHEXPANDER: INTERNAL ROOTFS, /USR ON USB.\n"
				"CHKROOT: COMPLETE ROOTFS ON USB, SHARED KERNEL.\n\n"
				"AN SSD OR HDD CAN BE FASTER; CHEAP USB STICKS MAY BE SLOW.");
			choice = choose_menu(app, welcome_title,
				mode_body,
				optional_modes, 2, 0,
				"ARROWS: SELECT   OK: CONTINUE");
			if (choice < 0 || stop_requested)
				return 2;
			chkroot = choice == 1;
		} else if (multiboot_config.policy == MULTIBOOT_REQUIRED) {
			snprintf(mode_body, sizeof(mode_body),
				"THIS 64 MB RECEIVER REQUIRES CHKROOT.\n\n"
				"INTERNAL FLASH: BOOTSTRAP AND SHARED KERNEL.\n"
				"USB: COMPLETE ENIGMA2 ROOTFS.");
			do {
				choice = choose_menu(app, welcome_title,
					mode_body,
					required_mode, 1, 0, "OK: CONTINUE");
				if (stop_requested)
					return 2;
			} while (choice != 0);
			chkroot = 1;
		}
		if (chkroot) {
			if (!configure_multiboot_storage(app, &multiboot_config,
				&multiboot_layout))
				return 2;
		} else if (!active && !configure_storage(app, uuid, sizeof(uuid))) {
			return 2;
		}
	}

	for (;;) {
		int install_result;
		if (!configure_network(app, interface, sizeof(interface), address,
			sizeof(address)))
			return 2;
		if (!network_synchronize_time(network_progress, app, error,
			sizeof(error))) {
			show_error(app, "TIME SYNCHRONIZATION FAILED", error);
			continue;
		}
		install_result = chkroot ?
			install_multiboot_image(app, &multiboot_config, &multiboot_layout) :
			install_packages(app);
		if (install_result == 1)
			break;
		if (install_result == 0)
			return 2;
	}
	if (!chkroot) {
		if (multiboot_config.single_core &&
			!network_disable_optional_services(network_progress, app, error,
				sizeof(error))) {
			show_error(app, "SINGLE-CORE BOOT PROFILE FAILED", error);
			return 1;
		}
		if (!write_done_marker(uuid, interface, address, error, sizeof(error))) {
			show_error(app, "COMPLETION FAILED", error);
			return 1;
		}
	} else {
		write_reboot_marker();
	}
	sync();
	if (chkroot)
		ui_screen(&app->ui, "CHKROOT SMALLBOX IS READY",
			"THE COMPLETE ENIGMA2 ROOTFS IS INSTALLED IN THE VERIFIED USB SLOT. THE INTERNAL KERNEL WAS NOT FLASHED. THE RECEIVER WILL NOW REBOOT INTO CHKROOT.",
			app->no_reboot ? "TEST MODE: OK EXITS WITHOUT REBOOT" :
			"DO NOT REMOVE THE USB DEVICE");
	else
		ui_screen(&app->ui, "SMALLBOX IS READY",
			"FLASHEXPANDER, 512 MB SWAP, NETWORK AND SMALLBOX PACKAGES ARE READY. THE RECEIVER WILL NOW REBOOT.",
			app->no_reboot ? "TEST MODE: OK EXITS WITHOUT REBOOT" :
			"DO NOT REMOVE THE USB DEVICE");
	if (app->no_reboot) {
		wait_acknowledge(app);
		return 0;
	}
	sleep(2);
	{
		char reboot_path[256];
		if (process_find("reboot", reboot_path, sizeof(reboot_path))) {
			char *argv[] = {reboot_path, NULL};
			process_run(argv, NULL, NULL, NULL);
		}
	}
	return 0;
}

static int list_devices(void)
{
	struct storage_device devices[STORAGE_MAX_DEVICES];
	char error[256];
	char size[32];
	int count = storage_scan_usb(devices, STORAGE_MAX_DEVICES, error,
		sizeof(error));
	int i;
	if (count < 0) {
		fprintf(stderr, "%s\n", error);
		return 1;
	}
	for (i = 0; i < count; ++i) {
		storage_format_size(devices[i].size_bytes, size, sizeof(size));
		printf("%s\t%s\t%s\tremovable=%d\n", devices[i].path,
			devices[i].model, size, devices[i].removable);
	}
	return 0;
}

static void usage(const char *program)
{
	printf("Usage: %s [--boot] [--no-reboot] [--list-devices] [--version]\n",
		program);
}

int main(int argc, char **argv)
{
	struct application app;
	int boot_mode = 0;
	int result;
	int i;
	memset(&app, 0, sizeof(app));
	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--boot") == 0)
			boot_mode = 1;
		else if (strcmp(argv[i], "--no-reboot") == 0)
			app.no_reboot = 1;
		else if (strcmp(argv[i], "--list-devices") == 0)
			return list_devices();
		else if (strcmp(argv[i], "--version") == 0) {
			printf("smallbox-wizard %s\n", VERSION);
			return 0;
		} else {
			usage(argv[0]);
			return 2;
		}
	}
	if (boot_mode && (access(DONE_MARKER, F_OK) == 0 ||
		access(REBOOT_MARKER, F_OK) == 0))
		return 0;
	if (geteuid() != 0) {
		fprintf(stderr, "smallbox-wizard must run as root\n");
		return 1;
	}
	install_signal_handlers();
	if (!ui_open(&app.ui)) {
		fprintf(stderr, "Framebuffer could not be opened: %s\n", strerror(errno));
		return 1;
	}
	input_open(&app.input);
	if (app.input.count == 0) {
		ui_error(&app.ui, "NO REMOTE CONTROL",
			"NO EVDEV DEVICE WITH ARROW AND OK BUTTONS WAS FOUND. CHECK /DEV/INPUT.");
		for (i = 0; i < 10 && app.input.count == 0; ++i) {
			sleep(1);
			input_open(&app.input);
		}
		if (app.input.count == 0) {
			ui_close(&app.ui);
			return 1;
		}
	}
	result = run_wizard(&app);
	input_close(&app.input);
	ui_close(&app.ui);
	return result;
}
