import os

mock_dir = "lib/simulator_mock/src"
def w(name, content):
    with open(os.path.join(mock_dir, name), "w") as f:
        f.write(content)

w("esp_sntp.h", """#pragma once
inline bool esp_sntp_enabled() { return true; }
inline void esp_sntp_setoperatingmode(int) {}
inline void esp_sntp_setservername(int, const char*) {}
inline void esp_sntp_init() {}
inline void esp_sntp_stop() {}
inline void sntp_set_time_sync_notification_cb(void (*)(struct timeval *)) {}
#define ESP_SNTP_OPMODE_POLL 0
#define SNTP_OPMODE_POLL 0

#define SNTP_SYNC_STATUS_COMPLETED 1
inline int sntp_get_sync_status() { return 1; }
""")

# add vTaskDelay and portTICK_PERIOD_MS
free_path = os.path.join(mock_dir, "freertos/FreeRTOS.h")
with open(free_path, "r") as f:
    free = f.read()
if "portTICK_PERIOD_MS" not in free:
    free += "\n#define portTICK_PERIOD_MS 1\n"
with open(free_path, "w") as f:
    f.write(free)

task_path = os.path.join(mock_dir, "freertos/task.h")
with open(task_path, "r") as f:
    task = f.read()
if "vTaskDelay" not in task:
    task += "\ninline void vTaskDelay(int) {}\n"
with open(task_path, "w") as f:
    f.write(task)

print("Fixed")
