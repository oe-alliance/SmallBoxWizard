#ifndef SMALLBOX_ABOUT_H
#define SMALLBOX_ABOUT_H

#include <signal.h>

#include "input.h"
#include "ui.h"

/* The components of the wizard with their licenses, OK shows the full text of one. */
void about(const struct ui_context *ui, struct input_context *input, const volatile sig_atomic_t *stop);

#endif
