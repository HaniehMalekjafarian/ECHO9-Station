#include "ollama_backend.h"

#include <curl/curl.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_HTTP_BODY (256u * 1024u)

struct http_body {
    char *data;
    size_t size;
    int exceeded;
};

static size_t collect_body(char *bytes, size_t size, size_t count, void *userdata) {
    struct http_body *body = userdata;
    if (size != 0 && count > (size_t)-1 / size) return 0;
    size_t incoming = size * count;
    if (incoming > MAX_HTTP_BODY - body->size) {
        body->exceeded = 1;
        return 0;
    }
    char *new_data = realloc(body->data, body->size + incoming + 1);
    if (!new_data) return 0;
    body->data = new_data;
    memcpy(body->data + body->size, bytes, incoming);
    body->size += incoming;
    body->data[body->size] = '\0';
    return incoming;
}

int ollama_infer(const char *prompt, char *output, size_t capacity) {
    if (!prompt || !output || capacity == 0) return -1;

    const char *url = getenv("OLLAMA_URL");
    if (!url || !*url) url = "http://host.docker.internal:11434/api/generate";
    const char *model = getenv("OLLAMA_MODEL");
    if (!model || !*model) model = "qwen2.5:0.5b";

    json_object *request = json_object_new_object();
    if (!request) return -1;
    json_object_object_add(request, "model", json_object_new_string(model));
    json_object_object_add(request, "prompt", json_object_new_string(prompt));
    json_object_object_add(request, "stream", json_object_new_boolean(0));
    json_object *options = json_object_new_object();
    if (!options) { json_object_put(request); return -1; }
    json_object_object_add(options, "num_predict", json_object_new_int(128));
    json_object_object_add(options, "temperature", json_object_new_double(0.4));
    json_object_object_add(request, "options", options);

    const char *payload = json_object_to_json_string_ext(request, JSON_C_TO_STRING_PLAIN);
    CURL *curl = curl_easy_init();
    struct curl_slist *headers = NULL;
    struct http_body body = {0};
    int success = -1;
    if (!curl) goto done;

    headers = curl_slist_append(headers, "Content-Type: application/json");
    if (!headers) goto done;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(payload));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 110L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (code != CURLE_OK || status != 200 || body.exceeded) {
        fprintf(stderr, "Ollama request failed: curl=%s, HTTP=%ld\n",
                curl_easy_strerror(code), status);
        goto done;
    }

    json_object *reply = json_tokener_parse(body.data ? body.data : "");
    if (!reply) goto done;
    json_object *response_text = NULL;
    if (json_object_object_get_ex(reply, "response", &response_text)
        && json_object_is_type(response_text, json_type_string)) {
        const char *text = json_object_get_string(response_text);
        size_t length = (size_t)json_object_get_string_len(response_text);
        if (length < capacity && !memchr(text, '\0', length)) {
            memcpy(output, text, length);
            output[length] = '\0';
            success = 0;
        }
    }
    json_object_put(reply);

done:
    free(body.data);
    curl_slist_free_all(headers);
    if (curl) curl_easy_cleanup(curl);
    json_object_put(request);
    return success;
}
