/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

#include "benchmark.h"
#include "esp_log.h"
#include "soundboard.h"
#include <stdio.h>

static const char *TAG = "benchmark";

/**
 * @brief Per-subsystem I/O counters
 */
typedef struct {
    int64_t  total_us, overall_us;        /**< Accumulated microseconds spent in fread/fwrite */
    size_t   total_bytes, overall_bytes;  /**< Accumulated bytes transferred */
    uint32_t total_calls,overall_calls;   /**< Number of fread/fwrite calls */
} benchmark_counters_t;

static benchmark_counters_t s_counters[BENCH_SUBSYSTEM_COUNT];

static const char *subsystem_names[] = {
    [BENCH_SD_READ]    = "SD_READ",
    [BENCH_I2S_WRITE]  = "I2C_WRITE",
    [BENCH_CACHE_LOAD] = "CACHE_LOAD",
    [BENCH_CACHE_HIT]  = "CACHE_HIT",
    [BENCH_MSC_READ]   = "MSC_READ",
    [BENCH_MSC_WRITE]  = "MSC_WRITE",
};

#define PRINT_COUNTERS_BUFSIZE 128

static int snprint_counters(char* buf, size_t size, benchmark_subsystem_t subsystem, bool overall, const char* context)
{
    if (subsystem >= BENCH_SUBSYSTEM_COUNT) {
        return 0;
    }
    benchmark_counters_t *c = &s_counters[subsystem];
    const char* name =  subsystem_names[subsystem];

    uint32_t call_count = (overall) ? c->overall_calls : c->total_calls;
    if (call_count == 0) {
        snprintf(buf, size, "%s: no data", name);
        return 0;

    }

    double kb = (double)(overall ? c->overall_bytes : c->total_bytes) / 1024.0;
    double sec = (double)(overall ? c->overall_us : c->total_us) / 1000000.0;
    double rate = sec > 0 ? kb / sec : 0;

    if (context != NULL) {
        return snprintf(buf, size, "%s: %.0f kB in %.3f s (%.0f kB/s, %"PRIu32" calls) [%s]",
                 name, kb, sec, rate, call_count, context);
    }
    return snprintf(buf, size, "%s: %.0f kB in %.3f s (%.0f kB/s, %"PRIu32" calls)",
                 name, kb, sec, rate, call_count);
}

static void reset_counters(int subsystem)
{
    if (subsystem >= BENCH_SUBSYSTEM_COUNT) {
        return;
    }
    benchmark_counters_t *c = &s_counters[subsystem];
    // do not reset overall_* counters
    c->total_us = 0;
    c->total_bytes = 0;
    c->total_calls = 0;
}

void benchmark_record(benchmark_subsystem_t subsystem, int64_t start_us, size_t bytes)
{
    if (subsystem >= BENCH_SUBSYSTEM_COUNT) {
        return;
    }
    benchmark_counters_t *c = &s_counters[subsystem];
    int64_t us = esp_timer_get_time() - start_us;
    c->total_us += us;
    c->overall_us += us;
    c->total_bytes += bytes;
    c->overall_bytes += bytes;
    c->total_calls++;
    c->overall_calls++;
}

void benchmark_log_and_reset(benchmark_subsystem_t subsystem, const char *context)
{
    char buf[PRINT_COUNTERS_BUFSIZE];
    snprint_counters(buf, PRINT_COUNTERS_BUFSIZE, subsystem, false, context);
    ESP_LOGD( TAG, "%s", buf );
    reset_counters(subsystem);
}

void benchmark_print_status(status_output_type_t output_type)
{
    if (output_type == STATUS_OUTPUT_COMPACT) {
        printf("[benchmark]");
        for (int i = 0; i < BENCH_SUBSYSTEM_COUNT; i++) {
            benchmark_counters_t *c = &s_counters[i];
            if (c->overall_calls == 0) continue;
            double kb = (double)c->overall_bytes / 1024.0;
            double sec = (double)c->overall_us / 1000000.0;
            double rate = sec > 0 ? kb / sec : 0;
            printf(" %s=%.0f kB/s", subsystem_names[i], rate);
        }
        printf("\n");
        return;
    }

    printf("IO stats:\n");
    char buf[PRINT_COUNTERS_BUFSIZE];
    for( int subsys=0; subsys<BENCH_SUBSYSTEM_COUNT; subsys += 1  ) {
       int n = snprint_counters(buf, PRINT_COUNTERS_BUFSIZE, subsys, true,  NULL);
       if(n>0 || (output_type >= STATUS_OUTPUT_VERBOSE)) {
           printf("  %s\n", buf);
       }
    }
}
