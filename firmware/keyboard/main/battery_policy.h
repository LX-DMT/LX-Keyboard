#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { BAT_LIGHT_NORMAL, BAT_LIGHT_OFF, BAT_LIGHT_CHECK } battery_light_state_t;
typedef struct {
    battery_light_state_t state;
    uint32_t generation,setting,sequence,last_ms,low_since,high_since,settle_since,check_since,peak_since,retry_after;
    uint16_t history[3],filtered,before_mv,sag_mv,retry_floor,off_mv,peak_load;
    uint8_t count,index;
    bool configured,awake,settled,failed,sag_known,disabled;
    unsigned trips,attempts,restores;
} battery_policy_t;

/* Pure bounded state machine; only the LED task calls the live instance. */
void battery_policy_step(battery_policy_t *p,uint32_t now,uint32_t sequence,bool valid,uint16_t mv,
                         bool awake,uint32_t setting,uint16_t peak_load,uint16_t applied_load,uint32_t applied_generation);
unsigned battery_policy_selftest(void);
uint16_t battery_policy_restore_mv(const battery_policy_t *p);
void battery_policy_set_enabled(battery_policy_t *p,bool enabled);
