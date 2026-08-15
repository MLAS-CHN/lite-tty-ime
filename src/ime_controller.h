#ifndef IME_CONTROLLER_H
#define IME_CONTROLLER_H

#include <sys/types.h>

void ime_controller_init();

bool ime_controller_handle_input_bytes(const char* buf, ssize_t n, int master_fd);

#endif // IME_CONTROLLER_H
