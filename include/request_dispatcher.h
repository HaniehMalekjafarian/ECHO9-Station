#ifndef AGENTOS_REQUEST_DISPATCHER_H
#define AGENTOS_REQUEST_DISPATCHER_H

#include <stddef.h>

/* The protocol decoder owns these strings.  A dispatcher must copy fields
 * it wishes to retain beyond request_dispatch_infer() returning. */
typedef struct {
    const char *request_id;
    const char *agent_id;
    const char *prompt;
} agentos_infer_request_t;

typedef enum {
    AGENTOS_DISPATCH_OK = 0,
    AGENTOS_DISPATCH_BACKEND_ERROR,
    AGENTOS_DISPATCH_DISABLED,
    AGENTOS_DISPATCH_TERMINATED,
    AGENTOS_DISPATCH_TABLE_FULL
} agentos_dispatch_result_t;

/* Called ONLY after a complete, valid operation="infer" request.
 * Instructor-owned.  This checkpoint still delegates directly to Ollama;
 * Phase 1 will attach the student ProcessManager here. */
agentos_dispatch_result_t request_dispatch_infer(
    const agentos_infer_request_t *request,
    char *answer,
    size_t capacity
);

#endif
