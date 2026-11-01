#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
void power_policy_init(uint8_t mode,esp_err_t (*shutdown_request)(void));
void power_policy_activity(void);
void power_policy_mouse(bool held,bool moved);
/* Raw SW71 / scan bit 69, active low; boot-low waits for a stable release. */
void power_policy_button(bool down);
/* Called by the existing low-priority controls task. */
void power_policy_tick(void);
bool power_policy_dark(void);
bool power_policy_shutting_down(void);
bool power_policy_commit_shutdown(void);
void power_policy_stats(void);
void power_policy_lights_test(void);
unsigned power_policy_selftest(void);
