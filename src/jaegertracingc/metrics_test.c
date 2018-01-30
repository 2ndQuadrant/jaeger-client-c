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

#include "unity.h"
#include "jaegertracingc/alloc.h"
#include "jaegertracingc/metrics.h"

void test_metrics()
{
    jaeger_counter* counter = jaeger_default_counter_init();
    TEST_ASSERT_NOT_NULL(counter);
    counter->inc(counter, 2);
    jaeger_global_alloc->free(jaeger_global_alloc, counter);

    counter = jaeger_null_counter_init();
    TEST_ASSERT_NOT_NULL(counter);
    counter->inc(counter, -1);
    jaeger_global_alloc->free(jaeger_global_alloc, counter);

    jaeger_gauge* gauge = jaeger_default_gauge_init();
    TEST_ASSERT_NOT_NULL(gauge);
    gauge->update(gauge, 3);
    jaeger_global_alloc->free(jaeger_global_alloc, gauge);

    gauge = jaeger_null_gauge_init();
    TEST_ASSERT_NOT_NULL(gauge);
    gauge->update(gauge, 4);
    jaeger_global_alloc->free(jaeger_global_alloc, gauge);
}