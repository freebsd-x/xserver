#pragma once
#include "xserver/ipc_socket.h"
#include "shared/protocol.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <cstdint>
#include <ctime>

namespace fbx::xserver {

class CalendarService {
public:
    // Called from xbus_dispatcher; returns true if message was handled.
    static bool ProcessMessage(std::shared_ptr<IpcSocket> client,
                               const shared::XBusMessage& msg) noexcept;

    // Called from compositor timer (~1 Hz) to fire due reminders via broadcast.
    static void pollReminders() noexcept;

    // Events that have a reminder, for com.freebsdx.Reminder: when the event
    // starts and when its reminder is due (start − reminder_min), local time.
    struct ReminderInfo {
        uint32_t    event_id;
        std::string title;
        std::time_t start;
        std::time_t due;
    };
    static std::vector<ReminderInfo> reminders() noexcept;

private:
    struct Event {
        uint32_t    id;
        uint16_t    year;
        uint8_t     month;    // 1–12
        uint8_t     day;      // 1–31
        uint8_t     hour;
        uint8_t     minute;
        uint8_t     end_hour;
        uint8_t     end_minute;
        uint16_t    reminder_min; // 0 = none
        uint8_t     all_day;
        std::string title;
        std::string description;
        std::string location;
    };

    static void   load() noexcept;
    static void   save() noexcept;
    static uint32_t nextId() noexcept;
    static std::string calendarPath() noexcept;

    // Returns count of events imported.
    static int importIcs(const std::string& path) noexcept;

    static std::mutex        mutex_;
    static std::vector<Event> events_;
    static bool              loaded_;
    static uint32_t          next_id_;
};

} // namespace fbx::xserver
