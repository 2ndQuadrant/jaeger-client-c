/*
 * Copyright (c) 2018 Uber Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * @file
 * Trace ID representation.
 */

#include "jaegertracingc/propagation.h"

#include <endian.h>

#include "jaegertracingc/internal/strings.h"
#include "jaegertracingc/metrics.h"
#include "jaegertracingc/options.h"
#include "jaegertracingc/span.h"

typedef struct extract_text_map_arg {
    jaeger_span_context* ctx;
    jaeger_metrics* metrics;
    const jaeger_headers_config* config;
    void (*normalize_key)(char* restrict, const char* restrict);
    void (*decode_value)(char* restrict, const char* restrict);
} extract_text_map_arg;

static opentracing_propagation_error_code
parse_key_value(jaeger_vector* baggage, char* str)
{
    char* eq_context;
    const char* key = strtok_r(str, "=", &eq_context);
    if (key == NULL) {
        return opentracing_propagation_error_code_span_context_corrupted;
    }
    const char* value = strtok_r(NULL, "=", &eq_context);
    if (value == NULL) {
        return opentracing_propagation_error_code_span_context_corrupted;
    }

    jaeger_key_value* kv = jaeger_vector_append(baggage);
    if (kv == NULL) {
        return opentracing_propagation_error_code_unknown;
    }
    if (!jaeger_key_value_init(kv, key, value)) {
        baggage->len--;
        return opentracing_propagation_error_code_unknown;
    }

    return opentracing_propagation_error_code_success;
}

static opentracing_propagation_error_code
parse_comma_separated_map(jaeger_vector* baggage, char* str)
{
    assert(baggage != NULL);
    assert(str != NULL);

    char* comma_context;
    char* kv_str = strtok_r(str, ",", &comma_context);
    while (kv_str != NULL) {
        const opentracing_propagation_error_code return_code =
            parse_key_value(baggage, kv_str);
        if (return_code != opentracing_propagation_error_code_success) {
            return return_code;
        }
        kv_str = strtok_r(NULL, ",", &comma_context);
    }
    return opentracing_propagation_error_code_success;
}

static opentracing_propagation_error_code
extract_text_map_callback(void* arg, const char* key, const char* value)
{
    jaeger_span_context* ctx = ((extract_text_map_arg*) arg)->ctx;
    const jaeger_headers_config* config = ((extract_text_map_arg*) arg)->config;
    void (*normalize_key)(char*, const char*) =
        ((extract_text_map_arg*) arg)->normalize_key;
    void (*decode_value)(char*, const char*) =
        ((extract_text_map_arg*) arg)->decode_value;
    char key_buffer[strlen(key) + 1];

    normalize_key(key_buffer, key);
    assert(ctx != NULL);
    if (strcmp(key_buffer, config->trace_context_header) == 0) {
        char value_buffer[strlen(value) + 1];
        decode_value(value_buffer, value);
        if (!jaeger_span_context_scan(ctx, value_buffer)) {
            return opentracing_propagation_error_code_span_context_corrupted;
        }
    }
    else if (strcmp(key_buffer, config->debug_header) == 0) {
        char value_buffer[strlen(value) + 1];
        decode_value(value_buffer, value);
        ctx->debug_id = jaeger_strdup(value_buffer);
        if (ctx->debug_id == NULL) {
            return opentracing_propagation_error_code_unknown;
        }
        ctx->flags |= jaeger_sampling_flag_debug | jaeger_sampling_flag_sampled;
    }
    else if (strcmp(key_buffer, config->baggage_header) == 0) {
        char value_buffer[strlen(value) + 1];
        decode_value(value_buffer, value);
        const opentracing_propagation_error_code return_code =
            parse_comma_separated_map(&ctx->baggage, value_buffer);
        if (return_code != opentracing_propagation_error_code_success) {
            return return_code;
        }
    }
    else {
        const int prefix_len = strlen(config->trace_baggage_header_prefix);
        const int key_len = strlen(key_buffer);
        if (key_len > prefix_len && memcmp(key_buffer,
                                           config->trace_baggage_header_prefix,
                                           prefix_len) == 0) {
            char suffix[key_len - prefix_len + 1];
            strncpy(suffix, key_buffer + prefix_len, sizeof(suffix));

            char value_buffer[strlen(value) + 1];
            decode_value(value_buffer, value);

            jaeger_key_value* kv = jaeger_vector_append(&ctx->baggage);
            if (kv == NULL) {
                return opentracing_propagation_error_code_unknown;
            }
            if (!jaeger_key_value_init(kv, suffix, value_buffer)) {
                ctx->baggage.len--;
                return opentracing_propagation_error_code_unknown;
            }
        }
    }
    return opentracing_propagation_error_code_success;
}

static inline opentracing_propagation_error_code
extract_from_text_map_helper(opentracing_text_map_reader* reader,
                             extract_text_map_arg* arg)
{
    opentracing_propagation_error_code result =
        opentracing_propagation_error_code_success;
    arg->ctx = jaeger_malloc(sizeof(*arg->ctx));
    if (arg->ctx == NULL || !jaeger_span_context_init(arg->ctx)) {
        result = opentracing_propagation_error_code_unknown;
        goto cleanup;
    }
    result = reader->foreach_key(reader, &extract_text_map_callback, arg);
    if (result != opentracing_propagation_error_code_success) {
        goto cleanup;
    }
    if (arg->ctx->trace_id.high == 0 && arg->ctx->trace_id.low == 0 &&
        arg->ctx->debug_id == NULL &&
        jaeger_vector_length(&arg->ctx->baggage) == 0) {
        /* Successfully decoded an empty span context. */
        result = opentracing_propagation_error_code_success;
        goto cleanup;
    }
    return opentracing_propagation_error_code_success;

cleanup:
    if (result == opentracing_propagation_error_code_span_context_corrupted &&
        arg->metrics != NULL) {
        arg->metrics->decoding_errors->inc(arg->metrics->decoding_errors, 1);
    }
    jaeger_span_context_destroy((jaeger_destructible*) arg->ctx);
    jaeger_free(arg->ctx);
    arg->ctx = NULL;
    return result;
}

opentracing_propagation_error_code
jaeger_extract_from_text_map(opentracing_text_map_reader* reader,
                             jaeger_span_context** ctx,
                             jaeger_metrics* metrics,
                             const jaeger_headers_config* config)
{
    assert(ctx != NULL);
    assert(config != NULL);
    extract_text_map_arg arg = {.ctx = NULL,
                                .config = config,
                                .metrics = metrics,
                                .normalize_key = &copy_str,
                                .decode_value = &copy_str};
    const opentracing_propagation_error_code result =
        extract_from_text_map_helper(reader, &arg);
    *ctx = arg.ctx;
    return result;
}

opentracing_propagation_error_code
jaeger_extract_from_http_headers(opentracing_http_headers_reader* reader,
                                 jaeger_span_context** ctx,
                                 jaeger_metrics* metrics,
                                 const jaeger_headers_config* config)
{
    assert(ctx != NULL);
    assert(config != NULL);
    extract_text_map_arg arg = {.ctx = NULL,
                                .config = config,
                                .metrics = metrics,
                                .normalize_key = &to_lowercase,
                                .decode_value = &decode_uri_value};
    const opentracing_propagation_error_code result =
        extract_from_text_map_helper((opentracing_text_map_reader*) reader,
                                     &arg);
    *ctx = arg.ctx;
    return result;
}

static inline bool read_binary(int (*callback)(void*, char*, size_t),
                               void* arg,
                               void* result,
                               int result_size)
{
    char buffer[result_size];
    const int num_read = callback(arg, buffer, sizeof(buffer));
    if (num_read != result_size) {
        return false;
    }
    uint64_t value = 0;
    for (int i = 0; i < num_read; i++) {
        value |= (buffer[i] << (i * CHAR_BIT));
    }
    switch (result_size) {
    case sizeof(uint64_t):
        *(uint64_t*) result = be64toh(value);
        break;
    case sizeof(uint32_t):
        *(uint32_t*) result = be32toh(value);
        break;
    default:
        assert(result_size == sizeof(uint8_t));
        *(uint8_t*) result = (uint8_t) value;
        break;
    }
    return true;
}

static opentracing_propagation_error_code parse_baggage_binary(
    int (*callback)(void*, char*, size_t), void* arg, jaeger_vector* baggage)
{
#define READ_BINARY(x)                                                        \
    do {                                                                      \
        if (!read_binary(callback, arg, &(x), sizeof((x)))) {                 \
            return opentracing_propagation_error_code_span_context_corrupted; \
        }                                                                     \
    } while (0)

#define READ_BUFFER(buffer, len)                                              \
    do {                                                                      \
        if (callback(arg, (buffer), (len)) != (int) (len)) {                  \
            return opentracing_propagation_error_code_span_context_corrupted; \
        }                                                                     \
        (buffer)[len] = '\0';                                                 \
    } while (0)

    uint32_t num_baggage_items;
    READ_BINARY(num_baggage_items);
    if (num_baggage_items > 0) {
        if (!jaeger_vector_reserve(baggage, num_baggage_items)) {
            return opentracing_propagation_error_code_unknown;
        }
        for (size_t i = 0; i < num_baggage_items; i++) {
            uint32_t key_len;
            READ_BINARY(key_len);
            char key_buffer[key_len + 1];
            READ_BUFFER(key_buffer, key_len);

            uint32_t value_len;
            READ_BINARY(value_len);
            char value_buffer[value_len + 1];
            READ_BUFFER(value_buffer, value_len);

            jaeger_key_value* kv = jaeger_vector_append(baggage);
            assert(kv != NULL); /* Should never be NULL if reserve succeeded. */
            if (!jaeger_key_value_init(kv, key_buffer, value_buffer)) {
                return opentracing_propagation_error_code_unknown;
            }
        }
    }

#undef READ_BINARY
#undef READ_BUFFER

    return opentracing_propagation_error_code_success;
}

opentracing_propagation_error_code
jaeger_extract_from_binary(int (*callback)(void*, char*, size_t),
                           void* arg,
                           jaeger_span_context** ctx,
                           jaeger_metrics* metrics,
                           const jaeger_headers_config* config)
{
    assert(callback != NULL);
    assert(ctx != NULL);
    assert(config != NULL);

    opentracing_propagation_error_code result =
        opentracing_propagation_error_code_success;
    *ctx = (jaeger_span_context*) jaeger_malloc(sizeof(jaeger_span_context));
    if (*ctx == NULL || !jaeger_span_context_init(*ctx)) {
        result = opentracing_propagation_error_code_unknown;
        goto cleanup;
    }

#define READ_BINARY(x)                                                     \
    do {                                                                   \
        if (!read_binary(callback, arg, &(x), sizeof((x)))) {              \
            result =                                                       \
                opentracing_propagation_error_code_span_context_corrupted; \
            goto cleanup;                                                  \
        }                                                                  \
    } while (0)

    READ_BINARY((*ctx)->trace_id.high);
    READ_BINARY((*ctx)->trace_id.low);
    READ_BINARY((*ctx)->span_id);
    READ_BINARY((*ctx)->parent_id);
    READ_BINARY((*ctx)->flags);

#undef READ_BINARY

    result = parse_baggage_binary(callback, arg, &(*ctx)->baggage);
    if (result != opentracing_propagation_error_code_success) {
        goto cleanup;
    }

    return opentracing_propagation_error_code_success;

cleanup:
    if (result == opentracing_propagation_error_code_span_context_corrupted &&
        metrics != NULL) {
        metrics->decoding_errors->inc(metrics->decoding_errors, 1);
    }
    jaeger_span_context_destroy((jaeger_destructible*) *ctx);
    jaeger_free(*ctx);
    *ctx = NULL;
    return result;
}

opentracing_propagation_error_code
jaeger_extract_from_custom(opentracing_custom_carrier_reader* reader,
                           jaeger_span_context** ctx,
                           jaeger_metrics* metrics,
                           const jaeger_headers_config* config);

opentracing_propagation_error_code
jaeger_inject_into_text_map(opentracing_text_map_writer* writer,
                            const jaeger_span_context* ctx,
                            jaeger_metrics* metrics,
                            const jaeger_headers_config* config);

opentracing_propagation_error_code
jaeger_inject_into_http_headers(opentracing_http_headers_writer* writer,
                                const jaeger_span_context* ctx,
                                jaeger_metrics* metrics,
                                const jaeger_headers_config* config);

opentracing_propagation_error_code
jaeger_inject_into_binary(opentracing_http_headers_writer* writer,
                          int (*callback)(void*, const char*, size_t),
                          jaeger_metrics* metrics,
                          const jaeger_headers_config* config);

opentracing_propagation_error_code
jaeger_inject_into_custom(opentracing_custom_carrier_writer* writer,
                          const jaeger_span_context* ctx,
                          jaeger_metrics* metrics,
                          const jaeger_headers_config* config);
