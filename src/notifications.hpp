#pragma once

#include "core.hpp"

#include <memory>
#include <vector>

namespace notifier {

class NotificationMonitor {
public:
    NotificationMonitor();
    ~NotificationMonitor();
    NotificationMonitor(const NotificationMonitor&) = delete;
    NotificationMonitor& operator=(const NotificationMonitor&) = delete;
    std::vector<Notification> poll(int timeout_ms = 0);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}