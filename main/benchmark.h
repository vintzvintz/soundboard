/*
 * SPDX-FileCopyrightText: 2025 Vincent (Soundboard Project)
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * @file benchmark.h
 * @brief I/O benchmark instrumentation — global API
 *
 * Provides per-subsystem timing counters for fread/fwrite calls.
 * Usage: call benchmark_record() around each fread/fwrite, then
 * benchmark_log_and_reset() at the end of each file transfer.
 *
 * All state is module-internal; callers only need the subsystem enum.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_timer.h"
#include "soundboard.h"

// to guard benchmark_*() calls in other modules with #ifdef
#define IO_STATS_ENABLE

/**
 * @brief I/O subsystem identifiers for benchmark tracking
 */
typedef enum {
    BENCH_SD_READ,       /**< SD card streaming reads (provider WAV file path) */
    BENCH_I2S_WRITE,
    BENCH_CACHE_LOAD,    /**< PSRAM cache loading reads (provider preload path) */
    BENCH_CACHE_HIT,     /**< PSRAM to Internal RAM memcpy() (cache hit)*/
    BENCH_MSC_READ,      /**< MSC USB read (fread from USB flash drive) */
    BENCH_MSC_WRITE,     /**< MSC SD write (fwrite to SD card) */
    BENCH_SUBSYSTEM_COUNT
} benchmark_subsystem_t;

/**
 * @brief Start a timing measurement
 * @return Start timestamp in microseconds
 */
static inline int64_t benchmark_start(void)
{
    return esp_timer_get_time();
}

/**
 * @brief Record a completed I/O operation
 *
 * @param subsystem  Which subsystem performed the I/O
 * @param start_us   Timestamp from benchmark_start()
 * @param bytes      Number of bytes transferred
 */
void benchmark_record(benchmark_subsystem_t subsystem, int64_t start_us, size_t bytes);

/**
 * @brief Log accumulated counters and reset them
 *
 * Logs at DEBUG level with "BENCHMARK" tag, then zeroes the counters.
 * No-op if call_count is zero.
 *
 * @param subsystem  Which subsystem to log and reset
 * @param context    Optional context string for log (e.g. filename), may be NULL
 */
void benchmark_log_and_reset(benchmark_subsystem_t subsystem, const char *context);

/**
 * @brief Print benchmark status information to console
 *
 * @param output_type Output verbosity level
 */
void benchmark_print_status(status_output_type_t output_type);
