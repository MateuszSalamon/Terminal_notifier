#include "calendar.hpp"
#include "core.hpp"

#include <curl/curl.h>
#include <libical/ical.h>

#include <algorithm>
#include <memory>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace notifier {
namespace {

using Component = std::unique_ptr<icalcomponent, decltype(&icalcomponent_free)>;

std::string value(const char* text) {
    return text ? plain_text(text) : "";
}

std::time_t local_wall_time(std::time_t timestamp) {
    std::tm broken_down{};
    gmtime_r(&timestamp, &broken_down);
    broken_down.tm_isdst = -1;
    return std::mktime(&broken_down);
}

std::time_t timestamp(icaltimetype time) {
    if (time.zone || icaltime_is_utc(time)) {
        return icaltime_as_timet(icaltime_convert_to_zone(time, icaltimezone_get_utc_timezone()));
    }
    return local_wall_time(icaltime_as_timet(time));
}

using Occurrence = std::pair<std::string, std::time_t>;

struct Expansion {
    std::vector<Event>* events;
    const std::set<Occurrence>* overrides;
    std::time_t from;
    std::time_t to;
    bool detached;
};

void collect(icalcomponent* component, icaltime_span* span, void* pointer) {
    auto& expansion = *static_cast<Expansion*>(pointer);
    const auto initial = icalcomponent_get_dtstart(component);
    const bool floating = !initial.zone && !icaltime_is_utc(initial);
    const auto start = floating ? local_wall_time(span->start) : span->start;
    const auto end = floating ? local_wall_time(span->end) : span->end;
    if (start >= expansion.to || (end > start ? end <= expansion.from : start < expansion.from)) return;
    if (!expansion.detached && expansion.overrides->count({value(icalcomponent_get_uid(component)), start})) return;
    expansion.events->push_back({start, end, initial.is_date != 0,
        value(icalcomponent_get_summary(component)), value(icalcomponent_get_location(component))});
}

std::size_t receive(char* data, std::size_t size, std::size_t count, void* pointer) {
    auto& content = *static_cast<std::string*>(pointer);
    const auto bytes = size * count;
    if (bytes > 8 * 1024 * 1024 - content.size()) return 0;
    try {
        content.append(data, bytes);
    } catch (...) {
        return 0;
    }
    return bytes;
}

}

std::vector<EventState> event_states(const std::vector<Event>& events, std::time_t now) {
    std::vector<EventState> states(events.size(), EventState::upcoming);
    std::time_t next_start = 0;
    bool has_next = false;
    for (std::size_t index = 0; index < events.size(); ++index) {
        const auto& event = events[index];
        if (event.end > event.start ? event.end <= now : event.start < now) {
            states[index] = EventState::past;
        } else if (event.start <= now) {
            states[index] = EventState::ongoing;
        } else if (!has_next || event.start < next_start) {
            next_start = event.start;
            has_next = true;
        }
    }
    if (has_next) {
        for (std::size_t index = 0; index < events.size(); ++index) {
            if (states[index] == EventState::upcoming && events[index].start == next_start) {
                states[index] = EventState::next;
            }
        }
    }
    return states;
}

bool CalendarActivityTracker::update(const std::vector<Event>& events, std::time_t now) {
    const auto states = event_states(events, now);
    std::set<Key> known;
    std::set<Key> ongoing;
    bool activity = false;
    for (std::size_t index = 0; index < events.size(); ++index) {
        if (states[index] == EventState::past) continue;
        const auto& event = events[index];
        const Key key{event.start, event.end, event.all_day, event.title, event.location};
        known.insert(key);
        if (initialized_ && !known_.count(key)) activity = true;
        if (states[index] == EventState::ongoing) {
            ongoing.insert(key);
            if (!ongoing_.count(key)) activity = true;
        }
    }
    known_ = std::move(known);
    ongoing_ = std::move(ongoing);
    initialized_ = true;
    return activity;
}

std::vector<Event> parse_calendar(const std::string& content, std::time_t from, std::time_t to) {
    Component calendar(icalparser_parse_string(content.c_str()), icalcomponent_free);
    if (!calendar || icalcomponent_isa(calendar.get()) != ICAL_VCALENDAR_COMPONENT ||
        icalcomponent_count_errors(calendar.get()) != 0) {
        throw std::runtime_error("The calendar response is not a valid ICS calendar");
    }
    std::set<Occurrence> overrides;
    for (auto* component = icalcomponent_get_first_component(calendar.get(), ICAL_VEVENT_COMPONENT);
         component; component = icalcomponent_get_next_component(calendar.get(), ICAL_VEVENT_COMPONENT)) {
        auto* recurrence = icalcomponent_get_first_property(component, ICAL_RECURRENCEID_PROPERTY);
        if (recurrence) {
            auto* range = icalproperty_get_first_parameter(recurrence, ICAL_RANGE_PARAMETER);
            if (range && icalparameter_get_range(range) == ICAL_RANGE_THISANDFUTURE) {
                throw std::runtime_error("This feed uses unsupported THISANDFUTURE recurrence changes");
            }
            const auto time = icalproperty_get_datetime_with_component(recurrence, component);
            overrides.emplace(value(icalcomponent_get_uid(component)), timestamp(time));
        }
    }
    std::vector<Event> events;
    for (auto* component = icalcomponent_get_first_component(calendar.get(), ICAL_VEVENT_COMPONENT);
         component; component = icalcomponent_get_next_component(calendar.get(), ICAL_VEVENT_COMPONENT)) {
        if (icalcomponent_get_status(component) == ICAL_STATUS_CANCELLED) continue;
        if (icaltime_is_null_time(icalcomponent_get_dtstart(component))) continue;
        for (auto* property = icalcomponent_get_first_property(component, ICAL_RRULE_PROPERTY);
             property; property = icalcomponent_get_next_property(component, ICAL_RRULE_PROPERTY)) {
            if (icalproperty_get_rrule(property).freq < ICAL_HOURLY_RECURRENCE) {
                throw std::runtime_error("Sub-hourly recurrence rules are not supported");
            }
        }
        Expansion expansion{&events, &overrides, from, to,
            icalcomponent_get_first_property(component, ICAL_RECURRENCEID_PROPERTY) != nullptr};
        icalcomponent_foreach_recurrence(component,
            icaltime_from_timet_with_zone(from - 2 * 86400, 0, icaltimezone_get_utc_timezone()),
            icaltime_from_timet_with_zone(to + 2 * 86400, 0, icaltimezone_get_utc_timezone()),
            collect, &expansion);
    }
    std::sort(events.begin(), events.end(), [](const Event& first, const Event& second) {
        return std::tie(first.start, first.end, first.title, first.location) <
               std::tie(second.start, second.end, second.title, second.location);
    });
    events.erase(std::unique(events.begin(), events.end(), [](const Event& first, const Event& second) {
        return first.start == second.start && first.end == second.end &&
               first.title == second.title && first.location == second.location;
    }), events.end());
    return events;
}

std::string download_calendar(const std::string& url) {
    if (url.rfind("https://", 0) != 0) throw std::runtime_error("Calendar URL must use HTTPS");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    if (!handle) throw std::runtime_error("Could not initialize the calendar downloader");
    std::string content;
    curl_easy_setopt(handle.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(handle.get(), CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(handle.get(), CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(handle.get(), CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(handle.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle.get(), CURLOPT_USERAGENT, "terminal-notifier/1.0");
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &content);
    const auto result = curl_easy_perform(handle.get());
    if (result != CURLE_OK) {
        throw std::runtime_error(std::string("Calendar download: ") + curl_easy_strerror(result));
    }
    long status = 0;
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) throw std::runtime_error("Calendar HTTP status " + std::to_string(status));
    return content;
}

}