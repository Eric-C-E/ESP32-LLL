#include "app_runtime.h"

#include <assert.h>

static app_runtime_t g_runtime;

app_runtime_t *app_runtime_get(void)
{
    return &g_runtime;
}

void app_runtime_init(void)
{
    g_runtime.button_events = xQueueCreate(16, sizeof(app_button_event_t));
    g_runtime.text_updates = xQueueCreate(8, sizeof(app_text_update_t));
    g_runtime.audio_chunks = xQueueCreate(8, sizeof(app_audio_chunk_t));
    g_runtime.state_bits = xEventGroupCreate();

    assert(g_runtime.button_events);
    assert(g_runtime.text_updates);
    assert(g_runtime.audio_chunks);
    assert(g_runtime.state_bits);
}
