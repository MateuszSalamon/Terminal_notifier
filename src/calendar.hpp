#pragma once

#include <ctime>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace notifier {

struct Event {
    std::time_t start = 0;
    std::time_t end = 0;
    bool all_day = false;
    std::string title;
    std::string location;
};

enum class EventState { past, upcoming, next, ongoing };

class CalendarActivityTracker {
public:
    bool update(const std::vector<Event>& events, std::time_t now);

private:
    using Key = std::tuple<std::time_t, std::time_t, bool, std::string, std::string>;
    std::set<Key> known_;
    std::set<Key> ongoing_;
    bool initialized_ = false;
};

std::vector<EventState> event_states(const std::vector<Event>& events, std::time_t now);
std::vector<Event> parse_calendar(const std::string& content, std::time_t from, std::time_t to);
std::string download_calendar(const std::string& url);

}