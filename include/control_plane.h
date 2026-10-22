#ifndef AGENTOS_CONTROL_PLANE_H
#define AGENTOS_CONTROL_PLANE_H
#include <stddef.h>
/* Trusted local admin protocol, independent of untrusted inference v1. */
int control_handle(const char *line,char *reply,size_t capacity);
#endif
