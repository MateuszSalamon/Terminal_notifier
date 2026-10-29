#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace notifier {

enum class Lane { teams, outlook, other };

class AttentionAlert {
public:
    using Clock = std::chrono::steady_clock;
    void trigger(Clock::time_point now);
    void acknowledge();
    bool pending() const;
    bool visible(Clock::time_point now) const;

private:
    bool pending_ = false;
    Clock::time_point started_{};
};

struct Notification {
    std::string app;
    std::string desktop_entry;
    std::string sender;
    std::uint32_t replaces_id = 0;
    std::string summary;
    std::string body;
    std::string origin;
    std::chrono::system_clock::time_point received = std::chrono::system_clock::now();
};

std::string plain_text(const std::string& text);
std::string notification_origin(const Notification& notification);
Lane classify(const Notification& notification,
              const std::vector<std::string>& teams_patterns = {},
              const std::vector<std::string>& outlook_patterns = {});
void remember(std::deque<Notification>& history, Notification notification);

}