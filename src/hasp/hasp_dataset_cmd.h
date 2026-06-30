/* MIT License - Copyright (c) 2019-2024 Francis Van Roie
   For full license information read the LICENSE file in the project folder */

#ifndef HASP_DATASET_CMD_H
#define HASP_DATASET_CMD_H

#include <stdint.h>

/*
 * SDBM hash constants for Dataset command attribute names.
 * Each value equals Parser::get_sdbm("<name>") — uint16_t, lowercase, digits skipped.
 *
 *   replace  = 11508    append   = 63034    clear    = 1069
 *   resize   = 2452     set_time = 20810
 */
#define DATASET_CMD_REPLACE  11508
#define DATASET_CMD_APPEND   63034
#define DATASET_CMD_CLEAR    1069
#define DATASET_CMD_RESIZE   2452
#define DATASET_CMD_SET_TIME 20810

/*
 * Process a Dataset attribute command dispatched from hasp_dispatch.cpp.
 *
 * id      — Dataset id parsed from the d<id> address prefix
 * attr    — Command name string ("replace", "append", "clear", "resize", "set_time")
 * payload — JSON command payload
 * update  — true = write (SET); false = read (GET), not supported for Dataset commands
 *
 * Called as:   hasp_process_dataset_attribute(id, attr, payload, update)
 * Dispatched by: dispatch_parse_dataset_attribute() in hasp_dispatch.cpp
 * Example:    d44.replace={"ser":0,"data":[26.7,26.8],"t_start":0,"t_step":3600}
 *             d44.append={"ser":0,"value":27.3,"t":1782731150}
 */
void hasp_process_dataset_attribute(uint8_t id, const char* attr, const char* payload, bool update);

#endif /* HASP_DATASET_CMD_H */
