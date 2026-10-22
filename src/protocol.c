#include "protocol.h"
#include "request_dispatcher.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

/* Validate and retrieve a nonempty string field. */
static int read_string(
    json_object *object,
    const char *key,
    const char **value,
    size_t limit
) {
    json_object *field = NULL;

    if (!json_object_object_get_ex(object, key, &field) ||
        !json_object_is_type(field, json_type_string)) {
        return 0;
    }

    const char *text = json_object_get_string(field);
    size_t length =
        (size_t)json_object_get_string_len(field);

    if (length == 0 || length > limit ||
        memchr(text, '\0', length)) {
        return 0;
    }

    *value = text;
    return 1;
}


/* json-c's strict tokener on our target builds rejects unescaped UTF-8
 * characters inside JSON strings. Normalize valid raw UTF-8 into ordinary
 * ASCII JSON unicode escapes BEFORE strict parsing, preserving all other
 * bytes and strict structural validation.
 *
 * A raw non-ASCII byte outside a string or a malformed UTF-8 sequence is
 * rejected. Escaped JSON sequences are left to the strict tokener.
 */
static char *normalize_json_utf8(const char *input)
{
    size_t len = strlen(input);
    if (len > (SIZE_MAX - 1) / 6)
        return NULL;

    char *out = malloc(len * 6 + 1);
    if (!out)
        return NULL;

    size_t i = 0, j = 0;
    int quoted = 0, escaped = 0;

    while (i < len) {
        unsigned char ch = (unsigned char)input[i];
        if (ch < 0x80) {
            out[j++] = (char)ch;
            i++;
            if (quoted) {
                if (escaped)
                    escaped = 0;
                else if (ch == '\\')
                    escaped = 1;
                else if (ch == '"')
                    quoted = 0;
            } else if (ch == '"') {
                quoted = 1;
            }
            continue;
        }

        if (!quoted || escaped)
            goto invalid;

        uint32_t cp;
        size_t width;
        if (ch >= 0xc2 && ch <= 0xdf) {
            cp = ch & 0x1f;
            width = 2;
        } else if (ch >= 0xe0 && ch <= 0xef) {
            cp = ch & 0x0f;
            width = 3;
        } else if (ch >= 0xf0 && ch <= 0xf4) {
            cp = ch & 0x07;
            width = 4;
        } else {
            goto invalid;
        }
        if (width > len - i)
            goto invalid;

        for (size_t k = 1; k < width; k++) {
            unsigned char next = (unsigned char)input[i + k];
            if ((next & 0xc0) != 0x80)
                goto invalid;
            cp = (cp << 6) | (next & 0x3f);
        }

        /* Reject overlong encodings, surrogates and invalid code points. */
        if ((width == 2 && cp < 0x80) ||
            (width == 3 && cp < 0x800) ||
            (width == 4 && cp < 0x10000) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp > 0x10ffff)
            goto invalid;

        if (cp <= 0xffff) {
            (void)snprintf(out + j, 7, "\\u%04x", (unsigned int)cp);
            j += 6;
        } else {
            cp -= 0x10000;
            unsigned int high = 0xd800 + (unsigned int)(cp >> 10);
            unsigned int low = 0xdc00 + (unsigned int)(cp & 0x3ff);
            (void)snprintf(out + j, 13, "\\u%04x\\u%04x", high, low);
            j += 12;
        }
        i += width;
    }

    out[j] = '\0';
    return out;

invalid:
    free(out);
    return NULL;
}

int protocol_handle_request(
    const char *line,
    char *output,
    size_t capacity
) {
    struct json_tokener *tokener = json_tokener_new();

    if (!tokener)
        return -1;

    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);

    char *normalized = normalize_json_utf8(line);
    json_object *request = NULL;
    int valid_json = 0;
    if (normalized) {
        size_t length = strlen(normalized);
        request = json_tokener_parse_ex(tokener, normalized, (int)length);
        valid_json =
            json_tokener_get_error(tokener) == json_tokener_success &&
            json_tokener_get_parse_end(tokener) == length &&
            json_object_is_type(request, json_type_object);
    }
    free(normalized);

    const char *id = NULL;
    const char *agent = NULL;
    const char *prompt = NULL;
    const char *code = "BAD_REQUEST";
    const char *message = "Invalid request fields or JSON";

    int valid = 0;

    if (valid_json) {
        const char *operation = NULL;

        json_object *version = NULL;
        json_object *payload = NULL;

        (void)read_string(
            request, "request_id", &id, 128
        );

        if (id &&
            json_object_object_get_ex(
                request, "version", &version
            ) &&
            json_object_is_type(
                version, json_type_int
            ) &&
            json_object_get_int64(version) == 1 &&
            read_string(
                request, "agent_id", &agent, 128
            ) &&
            read_string(
                request, "operation", &operation, 64
            ) &&
            json_object_object_get_ex(
                request, "payload", &payload
            ) &&
            json_object_is_type(
                payload, json_type_object
            ) &&
            read_string(
                payload, "prompt", &prompt, 2048
            )) {

            if (strcmp(operation, "infer") == 0) {
                valid = 1;
            } else {
                code = "UNSUPPORTED_OPERATION";
                message = "Only infer is supported in M1";
            }
        }
    }

    char answer[4096] = {0};

    /* Instructor-owned integration boundary: only a fully validated v1
     * inference request reaches this function.  The ProcessManager will
     * attach here in expanded P1; malformed requests cause no PCB effects.
     * Views are borrowed only until this synchronous call returns. */
    if (valid) {
        agentos_infer_request_t view = {
            .request_id = id,
            .agent_id = agent,
            .prompt = prompt
        };
        agentos_dispatch_result_t dispatched =
            request_dispatch_infer(&view, answer, sizeof(answer));
        if (dispatched != AGENTOS_DISPATCH_OK) {
            valid = 0;
            switch (dispatched) {
            case AGENTOS_DISPATCH_DISABLED:
                code = "AGENT_DISABLED";
                message = "Agent identity is disabled";
                break;
            case AGENTOS_DISPATCH_TERMINATED:
                code = "AGENT_TERMINATED";
                message = "Agent has terminated";
                break;
            case AGENTOS_DISPATCH_TABLE_FULL:
                code = "PROCESS_TABLE_FULL";
                message = "Agent process table is full";
                break;
            case AGENTOS_DISPATCH_BACKEND_ERROR:
            default:
                code = "BACKEND_ERROR";
                message = "Inference backend failed";
                break;
            }
        }
    }
    json_object *response = json_object_new_object();

    if (!response) {
        json_object_put(request);
        json_tokener_free(tokener);
        return -1;
    }

    json_object_object_add(
        response, "version", json_object_new_int(1)
    );

    json_object_object_add(
        response,
        "request_id",
        id ? json_object_new_string(id)
           : json_object_new_null()
    );

    json_object_object_add(
        response,
        "status",
        json_object_new_string(valid ? "ok" : "error")
    );

    json_object *detail = json_object_new_object();

    if (valid) {
	json_object_object_add(
             detail,
             "text",
             json_object_new_string(answer)
        );

        json_object_object_add(
            response, "result", detail
        );
    } else {
        json_object_object_add(
            detail,
            "code",
            json_object_new_string(code)
        );

        json_object_object_add(
            detail,
            "message",
            json_object_new_string(message)
        );

        json_object_object_add(
            response, "error", detail
        );
    }

    const char *json =
        json_object_to_json_string_ext(
            response, JSON_C_TO_STRING_PLAIN
        );

    int written = snprintf(
        output, capacity, "%s\n", json
    );

    /* JSON escaping can expand a backend answer considerably. For example,
     * one control character may take six bytes as \u00XX on the wire.
     * Never close the socket silently when that happens: return a bounded,
     * machine-readable error that preserves the request ID instead.
     */
    if (valid && written >= 0 && (size_t)written >= capacity) {
        json_object_object_del(response, "result");
        json_object_object_add(
            response, "status", json_object_new_string("error")
        );
        json_object *overflow = json_object_new_object();
        if (overflow) {
            json_object_object_add(
                overflow, "code", json_object_new_string("RESPONSE_TOO_LARGE")
            );
            json_object_object_add(
                overflow, "message",
                json_object_new_string("Inference result exceeds response limit")
            );
            json_object_object_add(response, "error", overflow);
            json = json_object_to_json_string_ext(
                response, JSON_C_TO_STRING_PLAIN
            );
            written = json ? snprintf(output, capacity, "%s\n", json) : -1;
        } else {
            written = -1;
        }
    }

    int result =
        written >= 0 && (size_t)written < capacity
            ? 0 : -1;

    json_object_put(response);
    json_object_put(request);
    json_tokener_free(tokener);

    return result;
}

