#include "calendar.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    setenv("TZ", "UTC", 1);
    tzset();
    const std::string content =
        "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Test//EN\r\n"
        "BEGIN:VEVENT\r\nUID:daily\r\nDTSTART:20261029T090000Z\r\n"
        "DTEND:20261029T100000Z\r\nRRULE:FREQ=DAILY;COUNT=4\r\n"
        "EXDATE:20261030T090000Z\r\nSUMMARY:Standup\\, planning\r\n"
        "LOCATION:Room A\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:daily\r\nRECURRENCE-ID:20261031T090000Z\r\n"
        "DTSTART:20261031T110000Z\r\nDTEND:20261031T120000Z\r\n"
        "SUMMARY:Moved standup\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:daily\r\nRECURRENCE-ID:20261101T090000Z\r\n"
        "STATUS:CANCELLED\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:holiday\r\nDTSTART;VALUE=DATE:20261030\r\n"
        "DTEND;VALUE=DATE:20261031\r\nSUMMARY:Company day\r\nEND:VEVENT\r\n"
        "BEGIN:VEVENT\r\nUID:cancelled\r\nDTSTART:20261029T120000Z\r\n"
        "STATUS:CANCELLED\r\nSUMMARY:Cancelled\r\nEND:VEVENT\r\n"
        "END:VCALENDAR\r\n";
    constexpr std::time_t from = 1793232000;
    const auto events = notifier::parse_calendar(content, from, from + 5 * 86400);
    check(events.size() == 3, "Recurrence, exclusions, overrides and cancellations");
    check(events[0].title == "Standup, planning", "ICS escaped comma");
    check(events[0].location == "Room A", "ICS location");
    check(events[1].all_day, "All-day event");
    check(events[2].title == "Moved standup", "Detached recurrence replacement");
    check(notifier::parse_calendar(content, from - 86400, from).empty(), "Calendar window");
    bool rejected = false;
    try { notifier::parse_calendar("<html>Login required</html>", from, from + 86400); }
    catch (const std::runtime_error&) { rejected = true; }
    check(rejected, "HTML is not silently treated as an empty calendar");
    setenv("TZ", "Europe/Warsaw", 1);
    tzset();
    const auto local = notifier::parse_calendar(
        "BEGIN:VCALENDAR\nVERSION:2.0\nBEGIN:VEVENT\nUID:floating\n"
        "DTSTART:20261029T090000\nDTEND:20261029T100000\nSUMMARY:Local\n"
        "END:VEVENT\nEND:VCALENDAR\n", from, from + 86400);
    check(local.size() == 1 && local[0].start == from + 8 * 3600, "Floating times use local timezone");
    const auto zoned = notifier::parse_calendar(
        "BEGIN:VCALENDAR\nVERSION:2.0\nBEGIN:VTIMEZONE\nTZID:Test/Fixed\n"
        "BEGIN:STANDARD\nDTSTART:19700101T000000\nTZOFFSETFROM:+0200\n"
        "TZOFFSETTO:+0200\nTZNAME:TEST\nEND:STANDARD\nEND:VTIMEZONE\n"
        "BEGIN:VEVENT\nUID:zoned\nDTSTART;TZID=Test/Fixed:20261029T090000\n"
        "DTEND;TZID=Test/Fixed:20261029T100000\nRRULE:FREQ=DAILY;COUNT=2\n"
        "SUMMARY:Folded \n title\nEND:VEVENT\n"
        "BEGIN:VEVENT\nUID:zoned\nRECURRENCE-ID;TZID=Test/Fixed:20261030T090000\n"
        "DTSTART;TZID=Test/Fixed:20261030T110000\nDTEND;TZID=Test/Fixed:20261030T120000\n"
        "SUMMARY:Moved zoned\nEND:VEVENT\nEND:VCALENDAR\n", from, from + 3 * 86400);
    check(zoned.size() == 2, "Zoned override suppresses its original occurrence");
    check(zoned[0].start == from + 7 * 3600 && zoned[0].title == "Folded title", "VTIMEZONE and line folding");
    check(zoned[1].start == from + 86400 + 9 * 3600, "Zoned moved occurrence timestamp");
    using notifier::Event;
    using notifier::EventState;
    const std::vector<Event> agenda = {
        {from + 7200, from + 10800, false, "Later", ""},
        {from - 1800, from, false, "Just ended", ""},
        {from - 900, from + 900, false, "Current", ""},
        {from + 600, from + 1200, false, "Next", ""},
        {from + 600, from + 1800, false, "Simultaneous next", ""},
        {from, from + 3600, false, "Starting now", ""},
        {from - 3600, from + 86400, true, "All-day current", ""}
    };
    const auto states = notifier::event_states(agenda, from);
    check(states == std::vector<EventState>{EventState::upcoming, EventState::past,
        EventState::ongoing, EventState::next, EventState::next, EventState::ongoing,
        EventState::ongoing}, "Unsorted agenda, overlaps, all-day events, ties and time boundaries");
    const auto transitioned = notifier::event_states(agenda, from + 1200);
    check(transitioned[0] == EventState::next && transitioned[2] == EventState::past &&
        transitioned[3] == EventState::past && transitioned[4] == EventState::ongoing,
        "Next and current events advance as time passes without a calendar refresh");
    check(notifier::event_states({}, from).empty(), "Empty agenda");
    check(notifier::event_states({agenda[1], agenda[2]}, from) ==
        std::vector<EventState>{EventState::past, EventState::ongoing}, "No next event when nothing is future");
    const Event instant{from, from, false, "Reminder", ""};
    check(notifier::event_states({instant}, from - 1)[0] == EventState::next &&
        notifier::event_states({instant}, from)[0] == EventState::ongoing &&
        notifier::event_states({instant}, from + 1)[0] == EventState::past,
        "Zero-duration event is current only at its start");
    notifier::CalendarActivityTracker activity;
    const Event meeting{from + 600, from + 1200, false, "Meeting", "Room A"};
    check(!activity.update({meeting}, from), "Initial future agenda establishes a baseline without an alert");
    check(!activity.update({meeting}, from + 1), "An unchanged future agenda does not trigger alerts");
    check(activity.update({meeting}, from + 600), "Calendar event start triggers activity");
    check(!activity.update({meeting}, from + 601), "Ongoing event is announced once, so acknowledgement persists");
    check(!activity.update({meeting}, from + 1200), "Event end does not trigger a new alert");
    const Event added{from + 1800, from + 2400, false, "Added meeting", "Room B"};
    check(activity.update({meeting, added}, from + 1201), "New calendar entry triggers activity after initial load");
    check(!activity.update({meeting, added}, from + 1202), "Identical calendar refresh does not re-arm alert");
    auto changed = added;
    changed.location = "Room C";
    check(activity.update({meeting, changed}, from + 1203), "Changed calendar entry triggers activity");
    check(!activity.update({changed}, from + 1204), "Removing an event does not trigger activity");
    check(activity.update({changed}, from + 1800), "Previously announced upcoming entry triggers again when it starts");
    notifier::CalendarActivityTracker startup;
    check(startup.update(agenda, from), "Already ongoing events trigger an alert at launch");
    check(!startup.update(agenda, from + 1), "Repeated ongoing events do not trigger duplicate alerts");
    std::cout << "Calendar tests passed\n";
}