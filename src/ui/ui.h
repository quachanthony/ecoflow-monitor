// Touchscreen UI: live stats plus AC/DC toggle buttons with
// pending/confirmed/failed feedback driven by real telemetry.
#pragma once

#include <stdint.h>

#include "../ef/connection.h"

namespace ui {

void begin();

// Redraws changed fields and handles touch. Call every loop iteration.
void tick(ef::Connection &conn, uint32_t now_ms);

}  // namespace ui
