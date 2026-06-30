/* MIT License - Copyright (c) 2019-2024 Francis Van Roie
   For full license information read the LICENSE file in the project folder */

#ifndef HASP_DATASET_H
#define HASP_DATASET_H

#include "hasplib.h"

#define HASP_DATASET_LABEL_LEN 17

/* ---------------------------------------------------------------------------
 * Consumer notification
 *
 * Any widget that wants to react to Dataset changes registers a callback.
 * The Dataset never knows which widget type is attached — it simply calls
 * every registered fn(obj) when its contents change.
 * --------------------------------------------------------------------------- */
typedef void (*hasp_dataset_notify_fn)(lv_obj_t* obj);

struct hasp_consumer_node_t {
    lv_obj_t*                 obj;
    hasp_dataset_notify_fn    fn;
    hasp_consumer_node_t*     _next;
};

/* ---------------------------------------------------------------------------
 * hasp_series_t
 *
 * Owns one runtime data series: color, label, a sample buffer, and time
 * metadata.  Buffer memory is managed by the Dataset API — callers must
 * never free buf directly.
 * --------------------------------------------------------------------------- */
struct hasp_series_t
{
    /* --- identity --- */
    lv_color_t color;
    char label[HASP_DATASET_LABEL_LEN];
    uint8_t    decimals; // scaling: stored = roundf(float_value * 10^decimals)

    /* --- sample buffer (lv_mem_alloc'd, nullptr when not yet allocated) --- */
    lv_coord_t* buf;      // sample data array
    uint16_t    buf_size; // allocated capacity (number of samples)
    uint16_t    count;    // number of valid samples currently stored

    /* --- time metadata (0 = unset) --- */
    uint32_t t_start; // unix timestamp of the oldest sample in buf
    uint32_t t_step;  // seconds between consecutive samples (0 = irregular)
};

/* ---------------------------------------------------------------------------
 * hasp_dataset_t
 *
 * Owns zero or more hasp_series_t instances.  The series array itself is
 * also lv_mem_alloc'd.  Datasets form a global singly-linked list; the
 * _next pointer is internal to the Dataset Manager.
 * --------------------------------------------------------------------------- */
struct hasp_dataset_t
{
    uint8_t                id;
    uint8_t                series_count;
    hasp_series_t*         series;      // dynamically allocated array of series
    hasp_consumer_node_t*  _consumers;  // registered notification consumers
    hasp_dataset_t*        _next;       // Dataset Manager internal — do not access directly
};

/* ===========================================================================
 * Dataset Manager API
 * =========================================================================== */

/* Create a new dataset with the given id.
 * Returns the existing entry unchanged if the id is already registered.
 * Returns nullptr on allocation failure. */
hasp_dataset_t* hasp_dataset_create(uint8_t id);

/* Destroy the dataset with the given id and free all owned memory.
 * Safe to call with an id that does not exist. */
void hasp_dataset_destroy(uint8_t id);

/* Find a previously created dataset by id.
 * Returns nullptr if not found. */
hasp_dataset_t* hasp_dataset_find(uint8_t id);

/* Destroy all datasets and reset the manager.
 * Call when reloading pages. */
void hasp_dataset_clear_all();

/* Parse the "series" JSON array from a dataset object definition and populate
 * the series layout (color, label, decimals).  Replaces any existing series. */
void hasp_dataset_parse_series(hasp_dataset_t* dataset, const JsonObject& config);

/* ===========================================================================
 * Consumer notification API
 * =========================================================================== */

/* Register obj as a consumer of ds.  fn is called with obj whenever the
 * dataset contents change.  Re-attaching the same obj updates fn. */
void hasp_dataset_attach(hasp_dataset_t* ds, lv_obj_t* obj, hasp_dataset_notify_fn fn);

/* Remove obj from the consumer list.  Safe to call if obj is not registered. */
void hasp_dataset_detach(hasp_dataset_t* ds, lv_obj_t* obj);

/* Call fn(obj) for every registered consumer.
 * Invoke after any write that changes dataset contents. */
void hasp_dataset_notify(hasp_dataset_t* ds);

/* ===========================================================================
 * Series access
 * =========================================================================== */

/* Return the number of series in the dataset (0 if dataset is nullptr). */
uint8_t hasp_dataset_series_count(const hasp_dataset_t* dataset);

/* Return a pointer to series[index], or nullptr if out of range. */
hasp_series_t* hasp_dataset_get_series(hasp_dataset_t* dataset, uint8_t index);

/* Convenience accessors — return default values when dataset/index are invalid. */
lv_color_t  hasp_dataset_series_color(const hasp_dataset_t* dataset, uint8_t index);
const char* hasp_dataset_series_label(const hasp_dataset_t* dataset, uint8_t index);

/* ===========================================================================
 * Buffer operations
 * =========================================================================== */

/* Allocate (or reallocate) the sample buffer to hold exactly size samples.
 * Existing data is discarded.  Passing size=0 frees the buffer.
 * Returns false on allocation failure. */
bool hasp_series_resize(hasp_series_t* series, uint16_t size);

/* Replace the entire buffer contents with count samples from data.
 * Auto-resizes the buffer when count exceeds current capacity.
 * Returns false on nullptr or allocation failure. */
bool hasp_series_replace(hasp_series_t* series, const lv_coord_t* data, uint16_t count);

/* Append one sample.  When the buffer is full the oldest sample is dropped
 * (shift-left semantics, matching LVGL chart behaviour).
 * Returns false when no buffer has been allocated (call hasp_series_resize first). */
bool hasp_series_append(hasp_series_t* series, lv_coord_t value);

/* Zero all samples and reset the count to 0 (buffer remains allocated). */
void hasp_series_clear(hasp_series_t* series);

/* Read-only pointer to the raw sample array (nullptr when unallocated). */
const lv_coord_t* hasp_series_buffer(const hasp_series_t* series);

/* Number of valid samples currently stored. */
uint16_t hasp_series_count(const hasp_series_t* series);

/* Allocated buffer capacity. */
uint16_t hasp_series_size(const hasp_series_t* series);

/* ===========================================================================
 * Time metadata
 * =========================================================================== */

/* Set regular-interval time metadata for the series buffer. */
void hasp_series_set_time(hasp_series_t* series, uint32_t t_start, uint32_t t_step);

/* Return the unix timestamp of the oldest sample (0 = unset). */
uint32_t hasp_series_t_start(const hasp_series_t* series);

/* Return the seconds-per-sample interval (0 = unset / irregular). */
uint32_t hasp_series_t_step(const hasp_series_t* series);

/* ===========================================================================
 * Dataset write API — the ONLY intended runtime data write paths.
 *
 * Each function modifies a series and then calls hasp_dataset_notify()
 * internally.  Callers never need to request a widget refresh manually.
 * Widgets must not call hasp_series_* directly for runtime writes.
 * =========================================================================== */

/* Replace all samples in series[idx].
 * data[] contains raw float values; scaling (10^series.decimals) is applied internally.
 * When t_start > 0 && t_step > 0, updates time metadata before notifying.
 * Returns false if idx is out of range or the buffer allocation fails. */
bool hasp_dataset_replace(hasp_dataset_t* ds, uint8_t idx,
                           const float* data, uint16_t count,
                           uint32_t t_start, uint32_t t_step);

/* Append one raw float sample to series[idx].
 * Scaling (10^series.decimals) is applied internally.
 * Auto-allocates the buffer (buf_size_hint slots) on first use.
 * When t_new > 0 and the series already has t_step set, recalculates t_start
 * so the timeline stays aligned with the newest sample. */
bool hasp_dataset_append(hasp_dataset_t* ds, uint8_t idx, float value,
                          uint16_t buf_size_hint, uint32_t t_new);

/* Reallocate series[idx] buffer to size slots, clearing any existing data. */
bool hasp_dataset_resize(hasp_dataset_t* ds, uint8_t idx, uint16_t size);

/* Zero all samples in series[idx] and reset count to 0. */
void hasp_dataset_clear_series(hasp_dataset_t* ds, uint8_t idx);

/* Update time metadata for series[idx] independently of sample data. */
void hasp_dataset_set_series_time(hasp_dataset_t* ds, uint8_t idx,
                                   uint32_t t_start, uint32_t t_step);

#endif
