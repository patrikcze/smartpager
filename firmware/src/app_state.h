// State shared between the UI task (Arduino loop, core 1) and the network task (core 0).
// Rule: g_store and g_status are only touched while holding g_lock.
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include "messaging/message_store.h"
#include "storage/config_store.h"
#include "ui/ui.h"

#ifndef SMARTPAGER_VERSION
#define SMARTPAGER_VERSION "0.2.1"
#endif

extern sp::MessageStore g_store;
extern sp::DeviceStatus g_status;
extern Config g_config;
extern SemaphoreHandle_t g_lock;
extern QueueHandle_t g_notifyQueue;  // NotifyMsg from network task to UI

struct NotifyMsg {
    uint32_t messageId;
    uint16_t count;
};

inline void appLock() { xSemaphoreTake(g_lock, portMAX_DELAY); }
inline void appUnlock() { xSemaphoreGive(g_lock); }
