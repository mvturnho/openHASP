/* MIT License - Copyright (c) 2019-2024 Francis Van Roie
   For full license information read the LICENSE file in the project folder */

#include "hasplib.h"
#include "hasp_dataset.h"

/* Singly-linked list of all live datasets.  New entries are prepended. */
static hasp_dataset_t* _head = nullptr;

/* ===========================================================================
 * Internal helpers
 * =========================================================================== */

/* Free a consumer linked list (nodes only — not the objects they point to). */
static void _free_consumers(hasp_consumer_node_t* head)
{
    while(head) {
        hasp_consumer_node_t* next = head->_next;
        lv_mem_free(head);
        head = next;
    }
}

/* Free the sample buffer of every series in the array. */
static void _free_series_buffers(hasp_series_t* series, uint8_t count)
{
    for(uint8_t i = 0; i < count; i++) {
        if(series[i].buf) {
            lv_mem_free(series[i].buf);
            series[i].buf      = nullptr;
            series[i].buf_size = 0;
            series[i].count    = 0;
        }
    }
}

/* Free all memory owned by a dataset node (series array + their buffers).
 * Does NOT free the node itself or adjust the linked list. */
static void _free_dataset_contents(hasp_dataset_t* ds)
{
    if(ds->series) {
        _free_series_buffers(ds->series, ds->series_count);
        lv_mem_free(ds->series);
        ds->series       = nullptr;
        ds->series_count = 0;
    }
    _free_consumers(ds->_consumers);
    ds->_consumers = nullptr;
}

/* ===========================================================================
 * Dataset Manager
 * =========================================================================== */

hasp_dataset_t* hasp_dataset_create(uint8_t id)
{
    hasp_dataset_t* existing = hasp_dataset_find(id);
    if(existing) return existing;

    hasp_dataset_t* ds = (hasp_dataset_t*)lv_mem_alloc(sizeof(hasp_dataset_t));
    if(!ds) {
        LOG_ERROR(TAG_HASP, F("Cannot allocate Dataset %u"), id);
        return nullptr;
    }

    memset(ds, 0, sizeof(hasp_dataset_t));
    ds->id    = id;
    ds->_next = _head;
    _head     = ds;
    return ds;
}

void hasp_dataset_destroy(uint8_t id)
{
    hasp_dataset_t* prev = nullptr;
    hasp_dataset_t* ds   = _head;

    while(ds) {
        if(ds->id == id) {
            /* Unlink from list */
            if(prev) prev->_next = ds->_next;
            else _head = ds->_next;

            _free_dataset_contents(ds);
            lv_mem_free(ds);
            return;
        }
        prev = ds;
        ds   = ds->_next;
    }
}

hasp_dataset_t* hasp_dataset_find(uint8_t id)
{
    for(hasp_dataset_t* ds = _head; ds != nullptr; ds = ds->_next) {
        if(ds->id == id) return ds;
    }
    return nullptr;
}

void hasp_dataset_clear_all()
{
    hasp_dataset_t* ds = _head;
    while(ds) {
        hasp_dataset_t* next = ds->_next;
        _free_dataset_contents(ds);
        lv_mem_free(ds);
        ds = next;
    }
    _head = nullptr;
}

/* ===========================================================================
 * Series layout — populated from JSONL "series" array
 * =========================================================================== */

void hasp_dataset_parse_series(hasp_dataset_t* dataset, const JsonObject& config)
{
    if(!dataset) return;

    JsonVariant set_val = config["series"];
    if(!set_val.is<JsonArray>()) return;

    JsonArray arr = set_val.as<JsonArray>();

    /* Count entries first so we can allocate the exact size. */
    uint8_t new_count = 0;
    for(JsonVariant v : arr) {
        (void)v;
        new_count++;
        if(new_count == 255) break; // guard uint8_t wrap
    }

    /* Release any previously allocated series. */
    if(dataset->series) {
        _free_series_buffers(dataset->series, dataset->series_count);
        lv_mem_free(dataset->series);
        dataset->series       = nullptr;
        dataset->series_count = 0;
    }

    if(new_count == 0) return;

    dataset->series = (hasp_series_t*)lv_mem_alloc(new_count * sizeof(hasp_series_t));
    if(!dataset->series) {
        LOG_ERROR(TAG_HASP, F("Cannot allocate series for Dataset %u"), dataset->id);
        return;
    }
    memset(dataset->series, 0, new_count * sizeof(hasp_series_t));

    uint8_t i = 0;
    for(JsonVariant v : arr) {
        if(i >= new_count) break;
        hasp_series_t& ser = dataset->series[i];

        if(v.is<JsonObject>()) {
            JsonObject sobj = v.as<JsonObject>();

            JsonVariant color_val = sobj["color"];
            if(!color_val.isNull()) {
                lv_color32_t c32;
                if(Parser::haspPayloadToColor(color_val.as<const char*>(), c32))
                    ser.color = lv_color_make(c32.ch.red, c32.ch.green, c32.ch.blue);
            }

            JsonVariant label_val = sobj["label"];
            if(!label_val.isNull())
                strncpy(ser.label, label_val.as<const char*>(), HASP_DATASET_LABEL_LEN - 1);

            JsonVariant dec_val = sobj["decimals"];
            if(!dec_val.isNull()) ser.decimals = dec_val.as<uint8_t>();

        } else if(v.is<const char*>()) {
            lv_color32_t c32;
            if(Parser::haspPayloadToColor(v.as<const char*>(), c32))
                ser.color = lv_color_make(c32.ch.red, c32.ch.green, c32.ch.blue);
        }

        i++;
    }

    dataset->series_count = i;
}

/* ===========================================================================
 * Series access
 * =========================================================================== */

uint8_t hasp_dataset_series_count(const hasp_dataset_t* dataset)
{
    return dataset ? dataset->series_count : 0;
}

hasp_series_t* hasp_dataset_get_series(hasp_dataset_t* dataset, uint8_t index)
{
    if(!dataset || !dataset->series || index >= dataset->series_count) return nullptr;
    return &dataset->series[index];
}

lv_color_t hasp_dataset_series_color(const hasp_dataset_t* dataset, uint8_t index)
{
    if(!dataset || !dataset->series || index >= dataset->series_count) return LV_COLOR_BLACK;
    return dataset->series[index].color;
}

const char* hasp_dataset_series_label(const hasp_dataset_t* dataset, uint8_t index)
{
    if(!dataset || !dataset->series || index >= dataset->series_count) return "";
    return dataset->series[index].label;
}

/* ===========================================================================
 * Buffer operations
 * =========================================================================== */

bool hasp_series_resize(hasp_series_t* series, uint16_t size)
{
    if(!series) return false;
    if(size == series->buf_size) return true;

    /* Free existing buffer first. */
    if(series->buf) {
        lv_mem_free(series->buf);
        series->buf      = nullptr;
        series->buf_size = 0;
        series->count    = 0;
    }

    if(size == 0) return true;

    series->buf = (lv_coord_t*)lv_mem_alloc(size * sizeof(lv_coord_t));
    if(!series->buf) return false;

    memset(series->buf, 0, size * sizeof(lv_coord_t));
    series->buf_size = size;
    series->count    = 0;
    return true;
}

bool hasp_series_replace(hasp_series_t* series, const lv_coord_t* data, uint16_t count)
{
    if(!series || !data) return false;

    /* Auto-grow the buffer when the incoming data is larger. */
    if(count > series->buf_size) {
        if(!hasp_series_resize(series, count)) return false;
    }

    memcpy(series->buf, data, count * sizeof(lv_coord_t));
    series->count = count;
    return true;
}

bool hasp_series_append(hasp_series_t* series, lv_coord_t value)
{
    if(!series || !series->buf || series->buf_size == 0) return false;

    if(series->count < series->buf_size) {
        series->buf[series->count++] = value;
    } else {
        /* Buffer full — drop oldest sample, shift left, append at end. */
        memmove(series->buf, series->buf + 1,
                (series->buf_size - 1) * sizeof(lv_coord_t));
        series->buf[series->buf_size - 1] = value;
        /* count stays at buf_size — all slots remain occupied */
    }
    return true;
}

void hasp_series_clear(hasp_series_t* series)
{
    if(!series || !series->buf) return;
    memset(series->buf, 0, series->buf_size * sizeof(lv_coord_t));
    series->count = 0;
}

const lv_coord_t* hasp_series_buffer(const hasp_series_t* series)
{
    return series ? series->buf : nullptr;
}

uint16_t hasp_series_count(const hasp_series_t* series)
{
    return series ? series->count : 0;
}

uint16_t hasp_series_size(const hasp_series_t* series)
{
    return series ? series->buf_size : 0;
}

/* ===========================================================================
 * Time metadata
 * =========================================================================== */

void hasp_series_set_time(hasp_series_t* series, uint32_t t_start, uint32_t t_step)
{
    if(!series) return;
    series->t_start = t_start;
    series->t_step  = t_step;
}

uint32_t hasp_series_t_start(const hasp_series_t* series)
{
    return series ? series->t_start : 0;
}

uint32_t hasp_series_t_step(const hasp_series_t* series)
{
    return series ? series->t_step : 0;
}

/* ===========================================================================
 * Consumer notification
 * =========================================================================== */

void hasp_dataset_attach(hasp_dataset_t* ds, lv_obj_t* obj, hasp_dataset_notify_fn fn)
{
    if(!ds || !obj || !fn) return;
    /* Update fn if already registered. */
    for(hasp_consumer_node_t* n = ds->_consumers; n; n = n->_next) {
        if(n->obj == obj) { n->fn = fn; return; }
    }
    hasp_consumer_node_t* node =
        (hasp_consumer_node_t*)lv_mem_alloc(sizeof(hasp_consumer_node_t));
    if(!node) return;
    node->obj          = obj;
    node->fn           = fn;
    node->_next        = ds->_consumers;
    ds->_consumers     = node;
}

void hasp_dataset_detach(hasp_dataset_t* ds, lv_obj_t* obj)
{
    if(!ds || !obj) return;
    hasp_consumer_node_t* prev = nullptr;
    hasp_consumer_node_t* n   = ds->_consumers;
    while(n) {
        if(n->obj == obj) {
            if(prev) prev->_next = n->_next;
            else     ds->_consumers = n->_next;
            lv_mem_free(n);
            return;
        }
        prev = n;
        n    = n->_next;
    }
}

void hasp_dataset_notify(hasp_dataset_t* ds)
{
    if(!ds) return;
    for(hasp_consumer_node_t* n = ds->_consumers; n; n = n->_next)
        if(n->fn) n->fn(n->obj);
}

/* ===========================================================================
 * Dataset write API (each modifies series data then notifies consumers)
 * =========================================================================== */

bool hasp_dataset_replace(hasp_dataset_t* ds, uint8_t idx,
                           const float* data, uint16_t count,
                           uint32_t t_start, uint32_t t_step)
{
    if(!ds || !data || idx >= ds->series_count) return false;
    hasp_series_t* ser = &ds->series[idx];
    if(t_start > 0 && t_step > 0) hasp_series_set_time(ser, t_start, t_step);

    if(count > ser->buf_size) {
        if(!hasp_series_resize(ser, count)) return false;
    }

    uint16_t scale = 1;
    for(uint8_t d = 0; d < ser->decimals; d++) scale *= 10;
    for(uint16_t i = 0; i < count; i++)
        ser->buf[i] = (lv_coord_t)roundf(data[i] * (float)scale);
    ser->count = count;

    hasp_dataset_notify(ds);
    return true;
}

bool hasp_dataset_append(hasp_dataset_t* ds, uint8_t idx, float value,
                          uint16_t buf_size_hint, uint32_t t_new)
{
    if(!ds || idx >= ds->series_count) return false;
    hasp_series_t* ser = &ds->series[idx];
    if(ser->buf_size == 0) {
        if(!hasp_series_resize(ser, buf_size_hint ? buf_size_hint : 10)) return false;
    }

    uint16_t scale = 1;
    for(uint8_t d = 0; d < ser->decimals; d++) scale *= 10;
    lv_coord_t scaled = (lv_coord_t)roundf(value * (float)scale);

    if(!hasp_series_append(ser, scaled)) return false;
    /* Recalculate t_start so the timeline stays aligned with the newest sample. */
    if(t_new > 0 && ser->t_step > 0) {
        uint32_t new_t_start = (ser->buf_size > 1)
                               ? (t_new - (uint32_t)(ser->buf_size - 1) * ser->t_step)
                               : t_new;
        hasp_series_set_time(ser, new_t_start, ser->t_step);
    }
    hasp_dataset_notify(ds);
    return true;
}

bool hasp_dataset_resize(hasp_dataset_t* ds, uint8_t idx, uint16_t size)
{
    if(!ds || idx >= ds->series_count) return false;
    if(!hasp_series_resize(&ds->series[idx], size)) return false;
    hasp_dataset_notify(ds);
    return true;
}

void hasp_dataset_clear_series(hasp_dataset_t* ds, uint8_t idx)
{
    if(!ds || idx >= ds->series_count) return;
    hasp_series_clear(&ds->series[idx]);
    hasp_dataset_notify(ds);
}

void hasp_dataset_set_series_time(hasp_dataset_t* ds, uint8_t idx,
                                   uint32_t t_start, uint32_t t_step)
{
    if(!ds || idx >= ds->series_count) return;
    hasp_series_set_time(&ds->series[idx], t_start, t_step);
    hasp_dataset_notify(ds);
}
