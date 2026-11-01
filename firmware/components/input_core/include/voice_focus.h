#pragma once
#include <stdbool.h>
typedef enum { LX_VOICE_IDLE,LX_VOICE_RECORDING,LX_VOICE_RECOGNIZING,LX_VOICE_CHAT } lx_voice_state_t;
typedef enum { LX_VOICE_PRESS,LX_VOICE_RELEASE,LX_VOICE_WAKE,LX_VOICE_DONE,LX_VOICE_CANCEL } lx_voice_event_t;
bool lx_voice_event(lx_voice_state_t *state,lx_voice_event_t event);
unsigned lx_voice_focus_selftest(void);
