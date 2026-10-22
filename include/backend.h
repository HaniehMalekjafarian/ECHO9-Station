
#ifndef AGENTOS_BACKEND_H
#define AGENTOS_BACKEND_H

#include <stddef.h>

/*
 * Execute an inference request.
 *
 * prompt: input supplied by an agent
 * output: caller-owned response buffer
 * capacity: total available buffer size
 *
 * Returns:
 *   0  on success
 *  -1  on failure
 */
int backend_infer(
    const char *prompt,
    char *output,
    size_t capacity
);

#endif

