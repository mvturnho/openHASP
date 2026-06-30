/* MIT License - Copyright (c) 2019-2024 Francis Van Roie
   For full license information read the LICENSE file in the project folder */

/*
 * Dataset command interface — the single runtime entry point for modifying
 * Dataset contents through the firmware command infrastructure.
 *
 * Address format:  d<id>.<cmd>=<json_payload>
 * Example:
 *   d44.replace={"ser":0,"data":[26.7,26.8,26.9],"t_start":1782727550,"t_step":3600}
 *   d44.append={"ser":0,"value":27.3,"t":1782731150}
 *   d44.clear={"ser":0}
 *   d44.resize={"ser":0,"size":100}
 *   d44.set_time={"ser":0,"t_start":1782727550,"t_step":3600}
 *
 * All command handlers call the Dataset write API (hasp_dataset_replace etc.)
 * which notifies registered consumers automatically.  Command handlers never
 * touch widgets directly.
 */

#include "hasplib.h"
#include "hasp_dataset.h"
#include "hasp_dataset_cmd.h"

/* ---------------------------------------------------------------------------
 * replace — {"ser":0,"data":[v1,v2,...],"t_start":0,"t_step":0}
 *          or bare array (ser=0 implied): [v1,v2,...]
 *
 * Values are raw floats.  Scaling is applied internally by hasp_dataset_replace
 * using the series.decimals field set in the Dataset definition.
 * --------------------------------------------------------------------------- */
static void hasp_dataset_cmd_replace(hasp_dataset_t* ds, const char* payload)
{
    size_t payload_len = strlen(payload);
    size_t maxsize     = JSON_ARRAY_SIZE(payload_len / 2 + 8) + JSON_OBJECT_SIZE(8) + 64;
    DynamicJsonDocument doc(maxsize);
    if(deserializeJson(doc, payload) != DeserializationError::Ok) {
        LOG_WARNING(TAG_HASP, F("Dataset %u replace: invalid JSON"), ds->id);
        return;
    }

    uint8_t  ser_num = 0;
    uint32_t t_start = 0;
    uint32_t t_step  = 0;
    JsonArray arr;

    if(doc.is<JsonObject>()) {
        JsonObject jo = doc.as<JsonObject>();
        ser_num = jo["ser"]     | (uint8_t)0;
        t_start = jo["t_start"] | (uint32_t)0;
        t_step  = jo["t_step"]  | (uint32_t)0;
        arr     = jo["data"].as<JsonArray>();
    } else {
        arr = doc.as<JsonArray>();
    }

    if(arr.isNull()) {
        LOG_WARNING(TAG_HASP, F("Dataset %u replace: missing 'data' array"), ds->id);
        return;
    }
    if(ser_num >= ds->series_count) {
        LOG_WARNING(TAG_HASP, F("Dataset %u: series index %u out of range (%u series)"),
                    ds->id, ser_num, ds->series_count);
        return;
    }

    uint16_t elem_count = 0;
    for(JsonVariant v : arr) { (void)v; elem_count++; }
    if(elem_count == 0) return;

    float* tmp = (float*)lv_mem_alloc(elem_count * sizeof(float));
    if(!tmp) {
        LOG_ERROR(TAG_HASP, F("Dataset %u replace: allocation failed"), ds->id);
        return;
    }
    uint16_t i = 0;
    for(JsonVariant v : arr) tmp[i++] = v.as<float>();

    hasp_dataset_replace(ds, ser_num, tmp, elem_count, t_start, t_step);
    lv_mem_free(tmp);

    LOG_VERBOSE(TAG_HASP, F("Dataset %u series %u: replaced %u samples"), ds->id, ser_num, elem_count);
}

/* ---------------------------------------------------------------------------
 * append — {"ser":0,"value":27.3,"t":1782731150,"size":100}
 *         or bare number (ser=0, no timestamp): 27.3
 *
 * "size" allocates the buffer on first append; ignored if buffer already exists.
 * "t"    unix timestamp of the new sample (0 = no time update).
 * Scaling is applied internally by hasp_dataset_append using series.decimals.
 * --------------------------------------------------------------------------- */
static void hasp_dataset_cmd_append(hasp_dataset_t* ds, const char* payload)
{
    uint8_t  ser_num   = 0;
    float    fvalue    = 0.0f;
    uint32_t t_new     = 0;
    uint16_t size_hint = 0;

    if(payload[0] == '{') {
        StaticJsonDocument<160> doc;
        if(deserializeJson(doc, payload) != DeserializationError::Ok) {
            LOG_WARNING(TAG_HASP, F("Dataset %u append: invalid JSON"), ds->id);
            return;
        }
        ser_num   = doc["ser"]  | (uint8_t)0;
        t_new     = doc["t"]    | (uint32_t)0;
        size_hint = doc["size"] | (uint16_t)0;
        if(doc.containsKey("value")) fvalue = doc["value"].as<float>();
        else                          fvalue = doc["val"].as<float>();
    } else {
        fvalue = (float)strtod(payload, nullptr);
    }

    if(ser_num >= ds->series_count) {
        LOG_WARNING(TAG_HASP, F("Dataset %u: series index %u out of range (%u series)"),
                    ds->id, ser_num, ds->series_count);
        return;
    }

    hasp_dataset_append(ds, ser_num, fvalue, size_hint, t_new);
}

/* ---------------------------------------------------------------------------
 * clear — {"ser":0}  or bare series index "0"  (ser=0 if payload is empty)
 * --------------------------------------------------------------------------- */
static void hasp_dataset_cmd_clear(hasp_dataset_t* ds, const char* payload)
{
    uint8_t ser_num = 0;

    if(payload && payload[0] == '{') {
        StaticJsonDocument<64> doc;
        if(deserializeJson(doc, payload) == DeserializationError::Ok)
            ser_num = doc["ser"] | (uint8_t)0;
    } else if(payload && payload[0] != '\0') {
        ser_num = (uint8_t)strtoul(payload, nullptr, DEC);
    }

    if(ser_num >= ds->series_count) {
        LOG_WARNING(TAG_HASP, F("Dataset %u: series index %u out of range (%u series)"),
                    ds->id, ser_num, ds->series_count);
        return;
    }
    hasp_dataset_clear_series(ds, ser_num);
    LOG_VERBOSE(TAG_HASP, F("Dataset %u series %u: cleared"), ds->id, ser_num);
}

/* ---------------------------------------------------------------------------
 * resize — {"ser":0,"size":100}
 * --------------------------------------------------------------------------- */
static void hasp_dataset_cmd_resize(hasp_dataset_t* ds, const char* payload)
{
    if(!payload || payload[0] != '{') {
        LOG_WARNING(TAG_HASP, F("Dataset %u resize: JSON payload required"), ds->id);
        return;
    }
    StaticJsonDocument<64> doc;
    if(deserializeJson(doc, payload) != DeserializationError::Ok) {
        LOG_WARNING(TAG_HASP, F("Dataset %u resize: invalid JSON"), ds->id);
        return;
    }
    uint8_t  ser_num = doc["ser"]  | (uint8_t)0;
    uint16_t size    = doc["size"] | (uint16_t)0;

    if(ser_num >= ds->series_count) {
        LOG_WARNING(TAG_HASP, F("Dataset %u: series index %u out of range (%u series)"),
                    ds->id, ser_num, ds->series_count);
        return;
    }
    if(size == 0) {
        LOG_WARNING(TAG_HASP, F("Dataset %u resize: size must be > 0"), ds->id);
        return;
    }
    hasp_dataset_resize(ds, ser_num, size);
    LOG_VERBOSE(TAG_HASP, F("Dataset %u series %u: resized to %u"), ds->id, ser_num, size);
}

/* ---------------------------------------------------------------------------
 * set_time — {"ser":0,"t_start":1782727550,"t_step":3600}
 * --------------------------------------------------------------------------- */
static void hasp_dataset_cmd_set_time(hasp_dataset_t* ds, const char* payload)
{
    if(!payload || payload[0] != '{') {
        LOG_WARNING(TAG_HASP, F("Dataset %u set_time: JSON payload required"), ds->id);
        return;
    }
    StaticJsonDocument<96> doc;
    if(deserializeJson(doc, payload) != DeserializationError::Ok) {
        LOG_WARNING(TAG_HASP, F("Dataset %u set_time: invalid JSON"), ds->id);
        return;
    }
    uint8_t  ser_num = doc["ser"]     | (uint8_t)0;
    uint32_t t_start = doc["t_start"] | (uint32_t)0;
    uint32_t t_step  = doc["t_step"]  | (uint32_t)0;

    if(ser_num >= ds->series_count) {
        LOG_WARNING(TAG_HASP, F("Dataset %u: series index %u out of range (%u series)"),
                    ds->id, ser_num, ds->series_count);
        return;
    }
    if(t_start == 0 || t_step == 0) {
        LOG_WARNING(TAG_HASP, F("Dataset %u set_time: t_start and t_step must be > 0"), ds->id);
        return;
    }
    hasp_dataset_set_series_time(ds, ser_num, t_start, t_step);
}

/* ---------------------------------------------------------------------------
 * Public dispatch entry point
 * --------------------------------------------------------------------------- */
void hasp_process_dataset_attribute(uint8_t id, const char* attr, const char* payload, bool update)
{
    if(!update) return;  // Dataset commands are write-only; GET is not supported

    hasp_dataset_t* ds = hasp_dataset_find(id);
    if(!ds) {
        LOG_WARNING(TAG_HASP, F("Dataset %u not found"), id);
        return;
    }

    uint16_t cmd_hash = Parser::get_sdbm(attr);
    switch(cmd_hash) {
        case DATASET_CMD_REPLACE:  hasp_dataset_cmd_replace(ds, payload);  break;
        case DATASET_CMD_APPEND:   hasp_dataset_cmd_append(ds, payload);   break;
        case DATASET_CMD_CLEAR:    hasp_dataset_cmd_clear(ds, payload);    break;
        case DATASET_CMD_RESIZE:   hasp_dataset_cmd_resize(ds, payload);   break;
        case DATASET_CMD_SET_TIME: hasp_dataset_cmd_set_time(ds, payload); break;
        default:
            LOG_WARNING(TAG_HASP, F("Dataset %u: unknown command '%s'"), id, attr);
    }
}
