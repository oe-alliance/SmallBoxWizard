#define _GNU_SOURCE

#include "about.h"
#include "input.h"
#include "multiboot.h"
#include "network.h"
#include "process.h"
#include "storage.h"
#include "ui.h"
#include "version.h"
#include "viewer.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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
#define WAIT "Please wait..."
#define KEEP_USB "Do not remove the USB device or switch off the receiver."

enum step {
	STEP_WELCOME,
	STEP_MODE,
	STEP_STORAGE,
	STEP_NETWORK,
	STEP_INSTALL,
	STEP_DONE,
	STEP_COUNT
};

/* What the summary card shows, "" while not chosen yet. */
struct setup {
	char receiver[64];
	char address[48];  /* Of eth0 at the start. */
	char mode[48];
	char usb[160];
	char network[96];
};

struct application {
	struct ui_context ui;
	struct input_context input;
	struct setup setup;
	char detail[512];
	const char *progress_title;
	int no_reboot;
	int demo;  /* Fake devices and work, nothing is changed. */
	int cleared;  /* ofgwrite had the framebuffer, LVGL draws everything again. */
	int busy;  /* No About while a long task runs. */
};

static struct application app;
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

static void summary(int on);

/* INFO over every screen that waits for a key. */
static int global_key(enum input_key key)
{
	if (key != INPUT_INFO || app.busy)
		return 0;
	app.busy = 1;
	summary(0);  /* The caller draws its screen with it again. */
	about(&app.ui, &app.input, &stop_requested);
	app.busy = 0;
	return 1;
}

static void key_pressed(enum input_key key)
{
	ui_key_pressed(input_key_name(key));
}

static void clock_tick(void)
{
	ui_clock(&app.ui);
}

static void process_idle(void *opaque)
{
	(void)opaque;
	ui_clock(&app.ui);
}

/* The steps on the left, the ones before current done. */
static void step(enum step current)
{
	static const char *const names[STEP_COUNT] = {"Welcome", "Mode", "USB storage", "Network", "Installation",
		"Done"};
	static const char *const icons[STEP_COUNT] = {UI_ICON_WELCOME, UI_ICON_MODE, UI_ICON_STORAGE, UI_ICON_NETWORK,
		UI_ICON_INSTALL, UI_ICON_FINISH};
	char marks[STEP_COUNT];
	for (int i = 0; i < STEP_COUNT; ++i) {
		marks[i] = 0;
		if (i < (int)current)
			marks[i] = 1;
		else if (i > (int)current)
			marks[i] = 2;
	}
	ui_sidebar(&app.ui, names, icons, STEP_COUNT, marks);
	ui_sidebar_select(&app.ui, current, 0);
}

static const char *or_dash(const char *text)
{
	return text[0] ? text : "\xE2\x80\x93";
}

/* The summary card above the screens where something is chosen, off for progress and errors. */
static void summary(int on)
{
	char text[640];
	const struct setup *s = &app.setup;
	if (!on) {
		ui_summary(&app.ui, "");
		return;
	}
	snprintf(text, sizeof(text), "Receiver\t%s\nIP (eth0)\t%s\nMode\t%s\nUSB\t%s\nNetwork\t%s", or_dash(s->receiver),
		or_dash(s->address), or_dash(s->mode), or_dash(s->usb), or_dash(s->network));
	ui_summary(&app.ui, text);
}

/* A text, or an error without the summary, until OK or BACK; drawn again after About. */
static void show(const char *title, const char *body, const char *footer, int error)
{
	enum input_key key;
	do {
		summary(!error);
		if (error)
			ui_error(&app.ui, title, body);
		else
			ui_screen(&app.ui, title, body, footer);
		key = input_next(&app.input, 1000);
	} while (key != INPUT_OK && key != INPUT_BACK && !stop_requested);
}

/* A choice of items, -1 for BACK. */
static int choose(const char *title, const char *body, const char *warning, const char *const items[], int count,
	int selected, const char *ok)
{
	char footer[160];
	if (count <= 0)
		return -1;
	if (selected < 0 || selected >= count)
		selected = 0;
	ui_keys(footer, sizeof(footer), &(struct ui_key_names){.arrows = "Select", .ok = ok, .back = "Back"});
	while (!stop_requested) {
		enum input_key key;
		summary(1);
		ui_menu_table(&app.ui, &(struct ui_menu){.title = title, .body = body, .warning = warning, .items = items,
			.count = count, .selected = selected, .marked = -1, .footer = footer});
		key = input_next(&app.input, 1000);
		selected = list_move(key, selected, count);
		if (key == INPUT_OK)
			return selected;
		if (key == INPUT_BACK)
			return -1;
	}
	return -1;
}

static void show_error(const char *title, const char *message)
{
	show(title, message, NULL, 1);
}

static void discard_pending_input(void)
{
	while (input_wait(&app.input, 0) != INPUT_NONE)
		;
}

/* The progress of the task in app.progress_title, for the callbacks of storage, network and multiboot. */
static void progress(int percent, const char *status, void *opaque)
{
	(void)opaque;
	discard_pending_input();
	if (app.cleared) {
		app.cleared = 0;
		ui_redraw(&app.ui);
	}
	snprintf(app.detail, sizeof(app.detail), "%s", status ? status : "");
	summary(0);
	ui_progress(&app.ui, app.progress_title, KEEP_USB, percent, app.detail, WAIT);
}

static void start_progress(const char *title)
{
	app.progress_title = title;
	progress(0, "", NULL);
}

static enum network_package_action package_failure(const struct network_package_failure *failure, void *opaque)
{
	const char *items[3];
	char body[1024];
	char skip_item[192];
	int count = 0;
	int skip_index = -1;
	int retry_index;
	int choice;

	(void)opaque;
	if (!failure)
		return NETWORK_PACKAGE_ABORT;
	discard_pending_input();
	if (failure->package[0]) {
		snprintf(skip_item, sizeof(skip_item), "Skip %.120s and continue", failure->package);
		skip_index = count;
		items[count++] = skip_item;
	}
	retry_index = count;
	items[count++] = "Retry the remaining packages";
	items[count++] = "Abort the installation";
	snprintf(body, sizeof(body),
		"opkg stopped with status %d.\n"
		"Package: %s\n"
		"Installed: %d of %d, remaining: %d, skipped: %d\n\n"
		"%.380s\n\nFull log: /.FlashExpander/.smallbox-opkg/install.log",
		failure->status, failure->package[0] ? failure->package : "not detected, see the log",
		failure->installed, failure->total, failure->remaining, failure->skipped,
		failure->detail[0] ? failure->detail : "No detailed error line was reported.");
	choice = choose("Package installation problem", body, NULL, items, count, 0, "Continue");
	start_progress(app.progress_title);  /* Back to the progress until the next callback. */
	if (choice == skip_index)
		return NETWORK_PACKAGE_SKIP;
	if (choice == retry_index)
		return NETWORK_PACKAGE_RETRY;
	return NETWORK_PACKAGE_ABORT;
}

static void multiboot_clear_display(void *opaque)
{
	(void)opaque;
	ui_clear(&app.ui, (struct ui_color){0, 0, 0, 255});
	ui_present(&app.ui);
	app.cleared = 1;
}

/* Demo mode */

static void pause_ms(long milliseconds)
{
	struct timespec moment = {milliseconds / 1000, milliseconds % 1000 * 1000000L};
	nanosleep(&moment, NULL);
}

/* Counts from 0 to 100 percent in about the given time, the statuses spread over it. */
static void demo_work(const char *const statuses[], int count, long milliseconds)
{
	for (int percent = 0; percent <= 100 && !stop_requested; percent += 2) {
		int index = percent * count / 101;
		progress(percent, statuses[index], NULL);
		pause_ms(milliseconds / 50);
	}
}

static int demo_install_packages(void)
{
	static const char *const statuses[] = {"Updating the package lists", "Installing enigma2",
		"Installing enigma2-plugin-systemplugins-networkwizard", "Installing the skin", "Configuring the packages"};
	struct network_package_failure failure = {.package = "enigma2-plugin-demo", .detail =
		"Demo: a package that cannot be downloaded.", .status = 255, .total = 663, .installed = 331, .remaining = 332};
	demo_work(statuses, 2, 3000);
	if (package_failure(&failure, NULL) == NETWORK_PACKAGE_ABORT)
		return 0;
	demo_work(statuses + 2, 3, 4000);
	return 1;
}

/* USB storage */

static void device_text(const struct storage_device *device, char *text, size_t size)
{
	char capacity[32];
	storage_format_size(device->size_bytes, capacity, sizeof(capacity));
	snprintf(text, size, "%s\t%s\t%s", device->path, device->model, capacity);
}

static int confirm_erase(const struct storage_device *device, int chkroot)
{
	const char *const first[] = {"No, go back", "Yes, erase all data"};
	const char *const second[] = {"Cancel", "Partition now"};
	char warning[384];
	char body[512];
	char capacity[32];
	storage_format_size(device->size_bytes, capacity, sizeof(capacity));
	snprintf(warning, sizeof(warning), "All data on %s (%s, %s) will be erased permanently.", device->path,
		device->model, capacity);
	snprintf(body, sizeof(body), chkroot ?
		"A FAT32 startup partition, an ext4 Chkroot partition and a 512 MB swap partition are created." :
		"An ext4 partition for /usr and a 512 MB swap partition are created.");
	if (choose("Erase the USB device?", body, warning, first, 2, 0, "Confirm") != 1)
		return 0;
	snprintf(warning, sizeof(warning), "Once started, %s must not be removed. A power failure can leave the "
		"receiver unusable until it is flashed again.", device->path);
	return choose("Final confirmation", NULL, warning, second, 2, 0, "Confirm") == 1;
}

/* The text above the list of USB devices. */
static const char *storage_hint(int count, int chkroot)
{
	if (!count)
		return chkroot ? "Connect a USB device with at least 2 GB, then scan again." :
			"Connect a USB device with at least 1 GB, then scan again.";
	if (chkroot)
		return "It will hold the complete Enigma2 root file system. Only real USB block devices are shown.";
	return "/usr of the receiver moves there. Only real USB block devices are shown.";
}

/* The USB devices with the fake one of the demo, at most UI_MAX_ITEMS - 1; -1 when the scan failed. */
static int scan_devices(struct storage_device devices[STORAGE_MAX_DEVICES], char *error, size_t error_size)
{
	int count = 0;
	/* The USB-storage/SCSI attach on Linux 3.2 and 3.14 can finish a few
	 * seconds after the physical hotplug event. A rescan therefore waits for
	 * the block device instead of immediately presenting an empty list. */
	for (int attempt = 0; attempt < 12 && !stop_requested; ++attempt) {
		struct timespec delay = {0, 250000000L};
		count = storage_scan_usb(devices, STORAGE_MAX_DEVICES, error, error_size);
		if (count != 0)
			break;
		if (attempt < 11)
			nanosleep(&delay, NULL);
	}
	if (count < 0)
		return -1;
	printf("[smallbox-wizard] USB scan: %d device%s found.\n", count, count == 1 ? "" : "s");
	for (int i = 0; i < count; ++i) {
		char label[192];
		device_text(&devices[i], label, sizeof(label));
		printf("[smallbox-wizard] USB scan: %s\n", label);
	}
	fflush(stdout);
	if (app.demo && count < STORAGE_MAX_DEVICES)
		devices[count++] = (struct storage_device){.name = "sdx", .path = "/dev/sdx",
			.model = "Demo USB stick", .size_bytes = 32ULL << 30, .removable = 1};
	return count > UI_MAX_ITEMS - 1 ? UI_MAX_ITEMS - 1 : count;
}

/* What the USB setup prepares: the UUID of FlashExpander or the layout of Chkroot. */
struct storage_target {
	const struct multiboot_config *config;
	int chkroot;
	char *uuid;
	size_t uuid_size;
	struct multiboot_layout *layout;
};

/* Partitions and prepares the chosen device, 1 when done. */
static int prepare_device(const struct storage_device *device, const struct storage_target *target, char *error,
	size_t error_size)
{
	int result;
	app.busy = 1;
	start_progress(target->chkroot ? "Setting up Chkroot on USB" : "Setting up the USB storage");
	if (app.demo) {
		static const char *const statuses[] = {"Creating the partitions", "Formatting", "Activating swap",
			"Copying /usr"};
		demo_work(statuses, target->chkroot ? 3 : 4, 5000);
		snprintf(target->uuid, target->uuid_size, "demo");
		result = 1;
	} else if (target->chkroot)
		result = multiboot_prepare(device, target->config, progress, NULL, target->layout, error, error_size);
	else
		result = storage_prepare(device, progress, NULL, target->uuid, target->uuid_size, error, error_size);
	app.busy = 0;
	return result;
}

/* Chooses and prepares the USB device. */
static int configure_storage(const struct storage_target *target)
{
	struct storage_device devices[STORAGE_MAX_DEVICES];
	char labels[UI_MAX_ITEMS][192];
	const char *items[UI_MAX_ITEMS];
	char error[512];
	for (;;) {
		const struct storage_device *device;
		char capacity[32];
		int choice;
		int count = scan_devices(devices, error, sizeof(error));
		if (count < 0) {
			show_error("USB detection failed", error);
			return 0;
		}
		for (int i = 0; i < count; ++i) {
			device_text(&devices[i], labels[i], sizeof(labels[i]));
			items[i] = labels[i];
		}
		items[count] = "Scan for USB devices again";
		choice = choose("Choose the USB device", storage_hint(count, target->chkroot), NULL, items, count + 1, 0,
			"Continue");
		if (stop_requested || choice < 0)
			return 0;
		if (choice == count)
			continue;
		device = &devices[choice];
		storage_format_size(device->size_bytes, capacity, sizeof(capacity));
		snprintf(app.setup.usb, sizeof(app.setup.usb), "%.96s, %s", device->model, capacity);
		if (!confirm_erase(device, target->chkroot)) {
			app.setup.usb[0] = '\0';
			continue;
		}
		if (prepare_device(device, target, error, sizeof(error)))
			return 1;
		app.setup.usb[0] = '\0';
		show_error("USB setup failed", error);
	}
}

/* Network */

/* The choices of the network screen: map is -2 for the existing connection, -3 to scan again, else the index of
 * the interface. Returns how many. */
static int network_items(const struct network_interface interfaces[], int count, const char *current_interface,
	const char *current_address, char labels[][160], const char *items[], int map[])
{
	int item_count = 0;
	if (current_interface[0]) {
		snprintf(labels[item_count], sizeof(labels[item_count]), "Use the existing connection\t%.31s\t%.47s",
			current_interface, current_address);
		items[item_count] = labels[item_count];
		map[item_count++] = -2;
	}
	for (int i = 0; i < count; ++i) {
		if (item_count >= UI_MAX_ITEMS - 1)
			break;
		if (interfaces[i].wireless)
			continue;
		snprintf(labels[item_count], sizeof(labels[item_count]), "LAN with DHCP\t%.31s\t%s", interfaces[i].name,
			interfaces[i].link ? "Cable connected" : "No link");
		items[item_count] = labels[item_count];
		map[item_count++] = i;
	}
	items[item_count] = "Scan for network interfaces again";
	map[item_count++] = -3;
	return item_count;
}

/* DHCP on the interface, 1 when it has an address. */
static int connect_interface(const struct network_interface *interface, char *chosen_address, size_t address_size,
	char *error, size_t error_size)
{
	int result = 1;
	app.busy = 1;
	start_progress("Setting up the network");
	if (app.demo) {
		static const char *const statuses[] = {"Requesting an address with DHCP"};
		demo_work(statuses, 1, 2000);
		snprintf(chosen_address, address_size, "%s", interface->address[0] ? interface->address :
			"192.168.0.99");  /* NOSONAR a made-up address of the demo */
	} else
		result = network_configure_dhcp(interface->name, progress, NULL, chosen_address, address_size, error,
			error_size);
	if (result)
		printf("[smallbox-wizard] Network: %s IPv4=%s%s\n", interface->name, chosen_address,
			app.demo ? " (demo)" : " (DHCP)");
	else
		fprintf(stderr, "[smallbox-wizard] Network: DHCP failed on %s: %s\n", interface->name, error);
	fflush(result ? stdout : stderr);
	app.busy = 0;
	return result;
}

static int configure_network(char *chosen_interface, size_t interface_size, char *chosen_address,
	size_t address_size)
{
	struct network_interface interfaces[NETWORK_MAX_INTERFACES];
	char labels[UI_MAX_ITEMS][160];
	const char *items[UI_MAX_ITEMS];
	char error[512];
	int map[UI_MAX_ITEMS];
	for (;;) {
		char current_interface[32] = "";
		char current_address[48] = "";
		int count = network_scan(interfaces, NETWORK_MAX_INTERFACES);
		int item_count;
		int choice;
		if (!network_has_ipv4(current_interface, sizeof(current_interface), current_address,
			sizeof(current_address)))
			current_interface[0] = '\0';
		item_count = network_items(interfaces, count, current_interface, current_address, labels, items, map);
		choice = choose("Set up the network",
			"The installation needs internet access. The wizard sets up wired LAN with DHCP, Wi-Fi can be set up "
			"later in Enigma2.", NULL, items, item_count, 0, "Connect");
		if (stop_requested || choice < 0)
			return 0;
		if (map[choice] == -3)
			continue;
		if (map[choice] == -2) {
			snprintf(chosen_interface, interface_size, "%s", current_interface);
			snprintf(chosen_address, address_size, "%s", current_address);
			printf("[smallbox-wizard] Network: %s IPv4=%s (already active)\n", current_interface,
				current_address);
			fflush(stdout);
			return 1;
		}
		if (connect_interface(&interfaces[map[choice]], chosen_address, address_size, error, sizeof(error))) {
			snprintf(chosen_interface, interface_size, "%s", interfaces[map[choice]].name);
			return 1;
		}
		show_error("Network setup failed", error);
	}
}

/* Installation */

/* 1 when installed, -1 to set up the network again, 0 to stop. */
static int install(const struct multiboot_config *config, const struct multiboot_layout *layout, int chkroot)
{
	const char *const items[] = {chkroot ? "Download and install the image" : "Install the SmallBox packages now",
		"Set up the network again"};
	char error[512];
	for (;;) {
		int choice;
		int result;
		choice = choose(chkroot ? "Install the Chkroot image" : "Install the packages", chkroot ?
			"The wizard downloads the SmallBox Multiboot image pinned by the server, verifies it and installs the "
			"root file system into USB slot 1. The internal kernel is not flashed." :
			"opkg downloads the SmallBox software and all its dependencies. /usr is already on the USB device.",
			NULL, items, 2, 0, "Start");
		if (stop_requested || choice < 0)
			return 0;
		if (choice == 1)
			return -1;
		app.busy = 1;
		start_progress(chkroot ? "Installing the Chkroot image" : "Installing the packages");
		snprintf(error, sizeof(error), "The installation was aborted.");
		if (app.demo && chkroot) {
			static const char *const statuses[] = {"Downloading the image", "Verifying the image",
				"Writing the root file system"};
			demo_work(statuses, 2, 3000);
			multiboot_clear_display(NULL);  /* Like ofgwrite, which owns the screen for a moment. */
			pause_ms(1500);
			demo_work(statuses + 2, 1, 2000);
			result = 1;
		} else if (app.demo)
			result = demo_install_packages();
		else if (chkroot)
			result = multiboot_install(config, layout, progress, multiboot_clear_display, NULL, error,
				sizeof(error));
		else
			result = network_install_smallbox(progress, package_failure, NULL, error, sizeof(error));
		app.busy = 0;
		if (result)
			return 1;
		show_error("Installation failed", error);
	}
}

static int write_done_marker(const char *uuid, const char *interface, const char *address, char *error,
	size_t error_size)
{
	char temporary[] = "/etc/smallbox-wizard.done.XXXXXX";
	int fd = mkstemp(temporary);
	FILE *file;
	time_t now = time(NULL);
	int saved;
	if (fd < 0) {
		snprintf(error, error_size, "The completion marker cannot be created: %s", strerror(errno));
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
	fprintf(file, "version=%s\ncompleted=%lld\nuuid=%s\ninterface=%s\nip=%s\n", VERSION, (long long)now,
		uuid ? uuid : "", interface ? interface : "", address ? address : "");
	saved = fflush(file) == 0 && fsync(fd) == 0;
	if (fclose(file) != 0)  /* Closed also after a failed flush. */
		saved = 0;
	if (!saved || rename(temporary, DONE_MARKER) != 0) {
		unlink(temporary);
		snprintf(error, error_size, "The completion marker could not be saved: %s", strerror(errno));
		return 0;
	}
	return 1;
}

static int eth0_ipv4(char *address, size_t address_size)
{
	struct network_interface interfaces[NETWORK_MAX_INTERFACES];
	int count = network_scan(interfaces, NETWORK_MAX_INTERFACES);
	address[0] = '\0';
	for (int i = 0; i < count; ++i)
		if (strcmp(interfaces[i].name, "eth0") == 0 && interfaces[i].address[0]) {
			snprintf(address, address_size, "%s", interfaces[i].address);
			return 1;
		}
	return 0;
}

static void write_reboot_marker(void)
{
	int fd = open(REBOOT_MARKER, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd >= 0) {
		if (write(fd, "chkroot\n", 8) == 8)
			fsync(fd);
		close(fd);
	}
}

/* The host name as the machine for the demo without a configuration, /proc/stb/info/model is dm8000 on some
 * receivers of other brands. */
static void demo_config(struct multiboot_config *config)
{
	memset(config, 0, sizeof(*config));
	config->policy = MULTIBOOT_OPTIONAL;
	if (gethostname(config->machine, sizeof(config->machine)) != 0 || !config->machine[0])
		snprintf(config->machine, sizeof(config->machine), "demo");
}

/* Continue with the USB storage of an earlier run, or disconnect it. 1 to continue, 0 to start over, -1 to stop. */
static int previous_setup(void)
{
	const char *const items[] = {"Continue the previous setup", "Start over with the USB setup"};
	char error[512];
	for (;;) {
		int choice = choose("Previous setup found",
			"The previous setup stopped after the USB storage was prepared. Continue keeps the active /usr and "
			"swap, start over disconnects them and returns to the USB selection.\n\n"
			"No data is erased before you confirm it again.", NULL, items, 2, 0, "Continue");
		if (stop_requested)
			return -1;
		if (choice == 0)
			return 1;
		if (choice < 0)
			continue;
		summary(0);
		ui_progress(&app.ui, "Resetting the setup", "The previous USB setup is disconnected safely.", 50,
			"No data is erased.", WAIT);
		if (storage_reset_expander(error, sizeof(error)))
			return 0;
		show_error("Reset failed", error);
	}
}

/* What the wizard sets up, filled step by step. */
struct wizard {
	struct multiboot_config config;
	struct multiboot_layout layout;
	char uuid[128];
	char interface[32];
	char address[48];
	char error[512];
	int chkroot;
};

/* The configuration and the start of the summary, 0 when it cannot be read. */
static int load_setup(struct wizard *w)
{
	struct setup *s = &app.setup;
	if (app.demo)
		demo_config(&w->config);
	else if (!multiboot_config_load(&w->config, w->error, sizeof(w->error))) {
		show_error("Wizard configuration", w->error);
		return 0;
	}
	snprintf(s->receiver, sizeof(s->receiver), "%s", w->config.machine_build[0] ? w->config.machine_build :
		w->config.machine);
	ui_header(&app.ui, app.demo ? "Demo mode" : "");  /* The receiver is in the summary. */
	if (eth0_ipv4(s->address, sizeof(s->address)))
		printf("[smallbox-wizard] Startup network: eth0 IPv4=%s\n", s->address);
	else
		printf("[smallbox-wizard] Startup network: eth0 has no IPv4 address.\n");
	fflush(stdout);
	return 1;
}

/* An earlier run prepared the USB storage: 1 to keep it, 0 to set it up again, -1 to stop. */
static int earlier_storage(struct wizard *w)
{
	int result;
	if (app.demo || !storage_is_expander_active(w->uuid, sizeof(w->uuid)))
		return 0;
	if (w->config.policy == MULTIBOOT_REQUIRED)
		return 0;
	result = previous_setup();
	if (result == 1) {
		snprintf(app.setup.mode, sizeof(app.setup.mode), "FlashExpander");
		snprintf(app.setup.usb, sizeof(app.setup.usb), "Prepared earlier");
	} else
		w->uuid[0] = '\0';
	return result;
}

/* The mode the configuration allows: 0 for FlashExpander, 1 for Chkroot, -1 for BACK. */
static int choose_mode(const struct multiboot_config *config)
{
	if (config->policy == MULTIBOOT_OPTIONAL) {
		const char *const modes[] = {"FlashExpander\t/usr on USB, the image stays in flash",
			"Chkroot Multiboot\tThe whole image on USB, the kernel stays in flash"};
		int choice = choose("How should the receiver use USB?",
			"An SSD or a hard disk is faster than a cheap USB stick. With Chkroot the USB device must "
			"stay connected.", NULL, modes, 2, 0, "Continue");
		return choice < 0 ? -1 : choice == 1;
	}
	if (config->policy == MULTIBOOT_REQUIRED) {
		const char *const required[] = {"Chkroot Multiboot\tThe whole image on USB"};
		int choice = choose("Chkroot is required",
			"The flash of this receiver holds only the bootstrap and the shared kernel. The complete "
			"Enigma2 root file system is installed on USB.", NULL, required, 1, 0, "Continue");
		return choice < 0 ? -1 : 1;
	}
	return 0;
}

/* Welcome, mode and USB storage until the storage is prepared, 0 to stop. */
static int setup_storage(struct wizard *w)
{
	char footer[96];
	ui_keys(footer, sizeof(footer), &(struct ui_key_names){.ok = "Start setup"});
	for (;;) {
		int mode;
		step(STEP_WELCOME);
		show("Welcome",
			"This wizard prepares the receiver for its small flash and memory. It has to be completed before "
			"Enigma2 can start.\n\nYou need a USB device, which will be erased, and a wired network with "
			"internet access.\n\nINFO shows the version and the licenses.", footer, 0);
		if (stop_requested)
			return 0;
		step(STEP_MODE);
		mode = choose_mode(&w->config);
		if (stop_requested)
			return 0;
		if (mode < 0)
			continue;
		w->chkroot = mode;
		snprintf(app.setup.mode, sizeof(app.setup.mode), w->chkroot ? "Chkroot Multiboot" : "FlashExpander");
		step(STEP_STORAGE);
		if (configure_storage(&(struct storage_target){&w->config, w->chkroot, w->uuid, sizeof(w->uuid),
			&w->layout}))
			return 1;
		if (stop_requested)
			return 0;
		app.setup.mode[0] = '\0';  /* BACK, to the welcome again. */
	}
}

/* Sets the clock for the downloads, 1 when it is right. */
static int set_clock(struct wizard *w)
{
	int result = 1;
	app.busy = 1;
	start_progress("Setting the clock");
	if (app.demo) {
		static const char *const statuses[] = {"Asking the time servers"};
		demo_work(statuses, 1, 1500);
	} else
		result = network_synchronize_time(progress, NULL, w->error, sizeof(w->error));
	app.busy = 0;
	if (!result)
		show_error("The clock could not be set", w->error);
	return result;
}

/* Network, clock and installation until installed, 0 to stop. */
static int network_and_install(struct wizard *w)
{
	for (;;) {
		int result;
		step(STEP_NETWORK);
		if (!configure_network(w->interface, sizeof(w->interface), w->address, sizeof(w->address)))
			return 0;
		snprintf(app.setup.network, sizeof(app.setup.network), "%s, %s", w->interface, w->address);
		if (!set_clock(w))
			continue;
		step(STEP_INSTALL);
		result = install(&w->config, &w->layout, w->chkroot);
		if (result >= 0)
			return result;
		app.setup.network[0] = '\0';
	}
}

/* The markers that end the setup, 0 when they could not be written. */
static int write_markers(struct wizard *w)
{
	if (app.demo)
		return 1;
	if (w->chkroot) {
		write_reboot_marker();
		return 1;
	}
	if (w->config.single_core && !network_disable_optional_services(progress, NULL, w->error, sizeof(w->error))) {
		show_error("Single-core boot profile failed", w->error);
		return 0;
	}
	if (!write_done_marker(w->uuid, w->interface, w->address, w->error, sizeof(w->error))) {
		show_error("Completion failed", w->error);
		return 0;
	}
	return 1;
}

static void reboot_receiver(void)
{
	char reboot_path[256];
	if (process_find("reboot", reboot_path, sizeof(reboot_path))) {
		char *argv[] = {reboot_path, NULL};
		process_run(argv, NULL, NULL, NULL);
	}
}

/* The last screen, then the restart unless in test or demo mode. */
static void finish(int chkroot)
{
	char footer[96];
	char countdown[96];
	const char *title = chkroot ? "Chkroot SmallBox is ready" : "SmallBox is ready";
	const char *body = chkroot ?
		"The complete Enigma2 root file system is installed in the verified USB slot. The internal kernel was "
		"not flashed. The receiver will restart automatically into Chkroot.\n\n" KEEP_USB :
		"FlashExpander, 512 MB swap, network and the SmallBox packages are ready. The receiver will restart "
		"automatically."
		"\n\n" KEEP_USB;
	step(STEP_DONE);
	if (app.no_reboot || app.demo) {
		ui_keys(footer, sizeof(footer), &(struct ui_key_names){.ok = "Exit"});
		show(title, body, footer, 0);
		return;
	}
	for (int remaining = 10; remaining > 0; --remaining) {
		snprintf(countdown, sizeof(countdown), "Restarting in %d second%s...", remaining,
			remaining == 1 ? "" : "s");
		summary(1);
		ui_screen(&app.ui, title, body, countdown);
		/* Keep LVGL's spinner moving while deliberately ignoring keys. */
		for (int tenth = 0; tenth < 10; ++tenth) {
			input_wait(&app.input, 100);
			ui_clock(&app.ui);
		}
	}
	summary(1);
	ui_screen(&app.ui, title, body, "Restarting now...");
	reboot_receiver();
}

static int run_wizard(void)
{
	static struct wizard w;
	int earlier;
	if (!load_setup(&w))
		return 1;
	step(STEP_WELCOME);
	earlier = earlier_storage(&w);
	if (earlier < 0)
		return 2;
	if (!earlier && !setup_storage(&w))
		return 2;
	if (!network_and_install(&w))
		return 2;
	if (!write_markers(&w))
		return 1;
	sync();
	finish(w.chkroot);
	return 0;
}

static int list_devices(void)
{
	struct storage_device devices[STORAGE_MAX_DEVICES];
	char error[256];
	char size[32];
	int count = storage_scan_usb(devices, STORAGE_MAX_DEVICES, error, sizeof(error));
	if (count < 0) {
		fprintf(stderr, "%s\n", error);
		return 1;
	}
	for (int i = 0; i < count; ++i) {
		storage_format_size(devices[i].size_bytes, size, sizeof(size));
		printf("%s\t%s\t%s\tremovable=%d\n", devices[i].path, devices[i].model, size, devices[i].removable);
	}
	return 0;
}

static void usage(const char *program)
{
	printf("Usage: %s [--boot] [--no-reboot] [--demo] [--list-devices] [--version]\n\n"
		"  --demo  shows every step with a fake USB device and fake work, nothing is changed\n", program);
}

int main(int argc, char **argv)
{
	int boot_mode = 0;
	int result;
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--boot") == 0)
			boot_mode = 1;
		else if (strcmp(argv[i], "--no-reboot") == 0)
			app.no_reboot = 1;
		else if (strcmp(argv[i], "--demo") == 0)
			app.demo = 1;
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
	if (boot_mode && !app.demo && (access(DONE_MARKER, F_OK) == 0 || access(REBOOT_MARKER, F_OK) == 0))
		return 0;
	if (geteuid() != 0) {
		fprintf(stderr, "smallbox-wizard must run as root\n");
		return 1;
	}
	install_signal_handlers();
	if (!ui_open(&app.ui)) {
		fprintf(stderr, "The framebuffer could not be opened: %s\n", strerror(errno));
		return 1;
	}
	input_open(&app.input);
	if (app.input.count == 0) {
		ui_error_keys(&app.ui, "No remote control",
			"No input device with arrow and OK keys was found. Check /dev/input.", WAIT);
		for (int i = 0; i < 10 && app.input.count == 0; ++i) {
			sleep(1);
			input_open(&app.input);
		}
		if (app.input.count == 0) {
			ui_close(&app.ui);
			return 1;
		}
	}
	input_set_global(global_key);
	input_set_press(key_pressed);
	input_set_idle(clock_tick);
	process_set_idle(process_idle, 100, NULL);
	result = run_wizard();
	process_set_idle(NULL, 0, NULL);
	input_close(&app.input);
	ui_clear(&app.ui, (struct ui_color){0, 0, 0, 255});
	ui_present(&app.ui);
	ui_close(&app.ui);
	return result;
}
