#pragma once
/* Development-only localhost fixture. No router, cloud or saved credentials. */
typedef struct chat_fixture chat_fixture_t;
chat_fixture_t *chat_fixture_start(void);
unsigned chat_fixture_stop(chat_fixture_t *s);
