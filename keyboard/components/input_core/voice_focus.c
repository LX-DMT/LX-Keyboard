#include "voice_focus.h"
bool lx_voice_event(lx_voice_state_t *s,lx_voice_event_t event)
{
    if(event==LX_VOICE_CANCEL) { *s=LX_VOICE_IDLE;return true; }
    if(*s==LX_VOICE_IDLE && event==LX_VOICE_PRESS) { *s=LX_VOICE_RECORDING;return true; }
    if(*s==LX_VOICE_IDLE && event==LX_VOICE_WAKE) { *s=LX_VOICE_CHAT;return true; }
    if(*s==LX_VOICE_RECORDING && event==LX_VOICE_RELEASE) { *s=LX_VOICE_RECOGNIZING;return true; }
    if((*s==LX_VOICE_CHAT || *s==LX_VOICE_RECOGNIZING) && event==LX_VOICE_DONE) { *s=LX_VOICE_IDLE;return true; }
    return false;
}
unsigned lx_voice_focus_selftest(void)
{
    lx_voice_state_t state=LX_VOICE_IDLE;unsigned errors=0;
#define CHECK(x) do { if(!(x)) errors++; } while(0)
    CHECK(lx_voice_event(&state,LX_VOICE_PRESS));
    CHECK(!lx_voice_event(&state,LX_VOICE_WAKE) && state==LX_VOICE_RECORDING);
    CHECK(lx_voice_event(&state,LX_VOICE_RELEASE) && state==LX_VOICE_RECOGNIZING);
    CHECK(!lx_voice_event(&state,LX_VOICE_WAKE) && !lx_voice_event(&state,LX_VOICE_PRESS));
    CHECK(lx_voice_event(&state,LX_VOICE_DONE) && state==LX_VOICE_IDLE);
    CHECK(lx_voice_event(&state,LX_VOICE_WAKE));
    CHECK(!lx_voice_event(&state,LX_VOICE_PRESS) && !lx_voice_event(&state,LX_VOICE_RELEASE) && state==LX_VOICE_CHAT);
    CHECK(lx_voice_event(&state,LX_VOICE_DONE));
    CHECK(!lx_voice_event(&state,LX_VOICE_RELEASE));
    CHECK(lx_voice_event(&state,LX_VOICE_PRESS) && lx_voice_event(&state,LX_VOICE_CANCEL) && state==LX_VOICE_IDLE);
#undef CHECK
    return errors;
}
