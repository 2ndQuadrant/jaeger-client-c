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

#include "jaegertracingc/sampler.h"

static bool jaeger_const_sampler_is_sampled(jaeger_sampler* sampler,
                                            const jaeger_trace_id* trace_id,
                                            const char* operation_name,
                                            jaeger_key_value_list* tags)
{
    (void) trace_id;
    (void) operation_name;
    jaeger_const_sampler* s = (jaeger_const_sampler*) sampler;
    if (tags != NULL) {
        jaeger_key_value_list_append(tags,
                                     JAEGERTRACINGC_SAMPLER_TYPE_TAG_KEY,
                                     JAEGERTRACINGC_SAMPLER_TYPE_CONST);
        jaeger_key_value_list_append(tags,
                                     JAEGERTRACINGC_SAMPLER_PARAM_TAG_KEY,
                                     (s->decision ? "true" : "false"));
    }
    return s->decision;
}

static void jaeger_sampler_noop_close(jaeger_sampler* sampler)
{
    (void) sampler;
}

void jaeger_const_sampler_init(jaeger_const_sampler* sampler, bool decision)
{
    assert(sampler != NULL);
    sampler->is_sampled = &jaeger_const_sampler_is_sampled;
    sampler->close = &jaeger_sampler_noop_close;
}

static bool
jaeger_probabilistic_sampler_is_sampled(jaeger_sampler* sampler,
                                        const jaeger_trace_id* trace_id,
                                        const char* operation_name,
                                        jaeger_key_value_list* tags)
{
    (void) trace_id;
    (void) operation_name;
    jaeger_probabilistic_sampler* s = (jaeger_probabilistic_sampler*) sampler;
#ifdef HAVE_RAND_R
    const double threshold = ((double) rand_r(&s->seed)) / RAND_MAX;
#else
    const double threshold = ((double) rand()) / RAND_MAX;
#endif /* HAVE_RAND_R */
    const bool decision = (s->probability >= threshold);
    if (tags != NULL) {
        jaeger_key_value_list_append(tags,
                                     JAEGERTRACINGC_SAMPLER_TYPE_TAG_KEY,
                                     JAEGERTRACINGC_SAMPLER_TYPE_PROBABILISTIC);
        jaeger_key_value_list_append(
            tags, JAEGERTRACINGC_SAMPLER_PARAM_TAG_KEY, s->probability_str);
    }
    return decision;
}

void jaeger_probabilistic_sampler_init(jaeger_probabilistic_sampler* sampler,
                                       double probability)
{
    assert(sampler != NULL);
    sampler->is_sampled = &jaeger_probabilistic_sampler_is_sampled;
    sampler->close = &jaeger_sampler_noop_close;
    sampler->probability =
        (probability < 0) ? 0 : ((probability > 1) ? 1 : probability);
    snprintf(&sampler->probability_str[0],
             sizeof(sampler->probability_str),
             "%f",
             sampler->probability);
}

static bool
jaeger_rate_limiting_sampler_is_sampled(jaeger_sampler* sampler,
                                        const jaeger_trace_id* trace_id,
                                        const char* operation_name,
                                        jaeger_key_value_list* tags)
{
    (void) trace_id;
    (void) operation_name;
    assert(sampler != NULL);
    jaeger_rate_limiting_sampler* s = (jaeger_rate_limiting_sampler*) sampler;
    const bool decision = jaeger_token_bucket_check_credit(&s->tok, 1);
    if (tags != NULL) {
        jaeger_key_value_list_append(tags,
                                     JAEGERTRACINGC_SAMPLER_TYPE_TAG_KEY,
                                     JAEGERTRACINGC_SAMPLER_TYPE_RATE_LIMITING);
        jaeger_key_value_list_append(tags,
                                     JAEGERTRACINGC_SAMPLER_PARAM_TAG_KEY,
                                     s->max_traces_per_second_str);
    }
    return decision;
}

void jaeger_rate_limiting_sampler_init(jaeger_rate_limiting_sampler* sampler,
                                       double max_traces_per_second)
{
    assert(sampler != NULL);
    sampler->is_sampled = &jaeger_rate_limiting_sampler_is_sampled;
    sampler->close = &jaeger_sampler_noop_close;
    jaeger_token_bucket_init(
        &sampler->tok,
        max_traces_per_second,
        (max_traces_per_second < 1) ? 1 : max_traces_per_second);
    snprintf(&sampler->max_traces_per_second_str[0],
             sizeof(sampler->max_traces_per_second_str),
             "%f",
             max_traces_per_second);
}

static bool jaeger_guaranteed_throughput_probabilistic_sampler_is_sampled(
    jaeger_sampler* sampler,
    const jaeger_trace_id* trace_id,
    const char* operation_name,
    jaeger_key_value_list* tags)
{
    (void) trace_id;
    (void) operation_name;
    assert(sampler != NULL);
    jaeger_guaranteed_throughput_probabilistic_sampler* s =
        (jaeger_guaranteed_throughput_probabilistic_sampler*) sampler;
    bool decision = s->probabilistic_sampler.is_sampled(
        (jaeger_sampler*) &s->probabilistic_sampler,
        trace_id,
        operation_name,
        NULL);
    if (decision) {
        s->lower_bound_sampler.is_sampled(
            (jaeger_sampler*) &s->lower_bound_sampler,
            trace_id,
            operation_name,
            NULL);
        if (tags != NULL) {
            jaeger_key_value_list_append(
                tags,
                JAEGERTRACINGC_SAMPLER_TYPE_TAG_KEY,
                JAEGERTRACINGC_SAMPLER_TYPE_PROBABILISTIC);
            jaeger_key_value_list_append(
                tags,
                JAEGERTRACINGC_SAMPLER_PARAM_TAG_KEY,
                s->probabilistic_sampler.probability_str);
        }
        return true;
    }
    decision = s->lower_bound_sampler.is_sampled(
        (jaeger_sampler*) &s->lower_bound_sampler,
        trace_id,
        operation_name,
        NULL);
    if (tags != NULL) {
        jaeger_key_value_list_append(tags,
                                     JAEGERTRACINGC_SAMPLER_TYPE_TAG_KEY,
                                     JAEGERTRACINGC_SAMPLER_TYPE_RATE_LIMITING);
        jaeger_key_value_list_append(
            tags,
            JAEGERTRACINGC_SAMPLER_PARAM_TAG_KEY,
            s->lower_bound_sampler.max_traces_per_second_str);
    }
    return decision;
}

void jaeger_guaranteed_throughput_probabilistic_sampler_close(
    jaeger_sampler* sampler)
{
    assert(sampler != NULL);
    jaeger_guaranteed_throughput_probabilistic_sampler* s =
        (jaeger_guaranteed_throughput_probabilistic_sampler*) sampler;
    s->probabilistic_sampler.close((jaeger_sampler*) &s->probabilistic_sampler);
    s->lower_bound_sampler.close((jaeger_sampler*) &s->lower_bound_sampler);
}

void jaeger_guaranteed_throughput_probabilistic_sampler_init(
    jaeger_guaranteed_throughput_probabilistic_sampler* sampler,
    double lower_bound,
    double sampling_rate)
{
    assert(sampler != NULL);
    sampler->is_sampled =
        &jaeger_guaranteed_throughput_probabilistic_sampler_is_sampled;
    sampler->close = &jaeger_guaranteed_throughput_probabilistic_sampler_close;
    jaeger_probabilistic_sampler_init(&sampler->probabilistic_sampler,
                                      sampling_rate);
    jaeger_rate_limiting_sampler_init(&sampler->lower_bound_sampler,
                                      lower_bound);
}

void jaeger_remotely_controlled_sampler_init(
    jaeger_remotely_controlled_sampler* sampler,
    char* service_name,
    jaeger_sampler* initial_sampler,
    int max_operations,
    const jaeger_duration* sampling_refresh_interval,
    jaeger_logger* logger,
    jaeger_metrics* metrics)
{
    assert(sampler != NULL);
    /* TODO */
}
