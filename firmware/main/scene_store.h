#ifndef OPEN_KEYLIGHT_SCENE_STORE_H
#define OPEN_KEYLIGHT_SCENE_STORE_H

#include "app.h"

/* Called once before the worker starts. Migrates legacy slots and seeds unused
 * slots without activating a scene. An unreadable record is never overwritten. */
esp_err_t app_scene_store_init(void);

#endif
