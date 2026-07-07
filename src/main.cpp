// EcoFlow River 2 Pro BLE monitor & control for the ESP32-2432S028R
// "Cheap Yellow Display". See README.md and PROTOCOL.md.
#include <Arduino.h>
#include <esp_task_wdt.h>

#include "config.h"
#include "ef/connection.h"
#include "secrets.h"
#include "ui/ui.h"

static ef::Connection conn;

void setup() {
  Serial.begin(115200);
  Serial.println("\n[main] EcoFlow River 2 monitor starting");

  // Hung BLE stack or a deadlock reboots the box instead of freezing it in
  // the van (Phase 7).
  esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
  esp_task_wdt_add(nullptr);  // watch the loop task

  ui::begin();
  conn.begin(ECOFLOW_USER_ID, ECOFLOW_DEVICE_SN);
}

void loop() {
  esp_task_wdt_reset();
  uint32_t now = millis();
  conn.tick(now);
  ui::tick(conn, now);
  delay(5);
}
