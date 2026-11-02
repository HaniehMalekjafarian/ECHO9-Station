#ifndef AGENTOS_OLLAMA_BACKEND_H
#define AGENTOS_OLLAMA_BACKEND_H

#include <stddef.h>

/* No global response buffers: output is owned by the caller. */
int ollama_infer(const char *prompt, char *output, size_t capacity);

#endif
