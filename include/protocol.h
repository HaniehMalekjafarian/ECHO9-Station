
#ifndef AGENTOS_PROTOCOL_H
#define AGENTOS_PROTOCOL_H

#include <stddef.h>

/*
 * Parse a request and create a JSON response.
 *
 * Returns 0 on success.
 * Returns -1 if the response cannot be created.
 *
 * The output includes a terminating newline.
 */
int protocol_handle_request(
    const char *line,
    char *output,
    size_t capacity
);

#endif

