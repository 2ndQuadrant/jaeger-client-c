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

#include "jaegertracingc/logging.h"

void test_logging()
{
    jaeger_logger std_logger;
    jaeger_std_logger_init(&std_logger);
    jaeger_set_logger(&std_logger);
    jaeger_log_error("Testing error logging");
    jaeger_log_warn("Testing warn logging");
    jaeger_log_info("Testing info logging");
    jaeger_set_logger(jaeger_null_logger());
}
