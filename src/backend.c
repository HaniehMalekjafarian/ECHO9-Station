#include "backend.h"
#include "ollama_backend.h"

/* Instructor-supplied Ollama integration: not a student implementation task. */
int backend_infer(const char *prompt, char *out, size_t capacity)
{
    return ollama_infer(prompt, out, capacity);
}
