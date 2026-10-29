#include "calendar.hpp"
#include "core.hpp"
#include "notifications.hpp"

#include <curl/curl.h>
#include <ncursesw/curses.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <clocale>
#include <csignal>
#include <condition_variable>
#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

volatile std::sig_atomic_t interrupted = 0;
void interrupt(int) { interrupted = 1; }

struct Options {
    std::string config;
    std::string calendar_url;
    std::string calendar_file;
    int refresh_seconds = 300;
    int calendar_days = 14;
    bool inspect = false;
    bool calendar_check = false;
    bool demo = false;
    bool no_calendar = false;
    std::vector<std::string> teams_patterns;
    std::vector<std::string> outlook_patterns;
};

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

int number(const std::string& text, int minimum, int maximum) {
    std::size_t consumed = 0;
    const int result = std::stoi(text, &consumed);
    if (consumed != text.size() || result < minimum || result > maximum) {
        throw std::runtime_error("Number outside allowed range");
    }
    return result;
}

void read_config(Options& options, bool required) {
    std::ifstream input(options.config);
    if (!input) {
        if (required) throw std::runtime_error("Cannot open the requested configuration file");
        return;
    }
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line.front() == '#') continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos) throw std::runtime_error("Expected key=value in configuration");
        const auto key = trim(line.substr(0, equals));
        const auto setting = trim(line.substr(equals + 1));
        if (key == "calendar_url") options.calendar_url = setting;
        else if (key == "refresh_seconds") options.refresh_seconds = number(setting, 30, 86400);
        else if (key == "calendar_days") options.calendar_days = number(setting, 1, 90);
        else if (key == "teams_match") options.teams_patterns.push_back(setting);
        else if (key == "outlook_match") options.outlook_patterns.push_back(setting);
        else throw std::runtime_error("Unknown configuration key: " + notifier::plain_text(key));
    }
}

void usage() {
    std::cout <<
        "terminal-notifier [options]\n\n"
        "  --config FILE          Read a specific private configuration file\n"
        "  --calendar-file FILE   Read a local ICS file instead of downloading\n"
        "  --no-calendar          Disable calendar fetching\n"
        "  --refresh SECONDS      Calendar refresh interval (30..86400)\n"
        "  --days DAYS            Agenda window (1..90, default 14)\n"
        "  --teams-match TEXT     Match a Teams PWA identifier (repeatable)\n"
        "  --outlook-match TEXT   Match an Outlook PWA identifier (repeatable)\n"
        "  --inspect              Print notification source metadata only\n"
        "  --check-calendar       Fetch/parse calendar and print only event count\n"
        "  --demo                 Offline sample panes, no bus or network access\n"
        "  --help                 Show this help\n\n"
        "Keys: Tab or 1/2/3 select pane; arrows/PgUp/PgDn scroll;\n"
        "Home/End jump; r refreshes calendar; c clears notifications; a acknowledges alerts; q quits.\n";
}

Options arguments(int argc, char** argv) {
    Options options;
    bool explicit_config = false;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) options.config = std::string(xdg) + "/terminal-notifier/config";
    else if (const char* home = std::getenv("HOME")) options.config = std::string(home) + "/.config/terminal-notifier/config";
    if (access("terminal-notifier.conf", R_OK) == 0) options.config = "terminal-notifier.conf";
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") { usage(); std::exit(0); }
        if (argument == "--config") {
            if (++index >= argc) throw std::runtime_error("--config requires a file");
            options.config = argv[index];
            explicit_config = true;
        }
    }
    read_config(options, explicit_config);
    if (const char* url = std::getenv("TERMINAL_NOTIFIER_ICS_URL")) options.calendar_url = url;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto next = [&]() -> std::string {
            if (++index >= argc) throw std::runtime_error(argument + " requires a value");
            return argv[index];
        };
        if (argument == "--config") next();
        else if (argument == "--calendar-file") options.calendar_file = next();
        else if (argument == "--refresh") options.refresh_seconds = number(next(), 30, 86400);
        else if (argument == "--days") options.calendar_days = number(next(), 1, 90);
        else if (argument == "--teams-match") options.teams_patterns.push_back(next());
        else if (argument == "--outlook-match") options.outlook_patterns.push_back(next());
        else if (argument == "--inspect") options.inspect = true;
        else if (argument == "--check-calendar") options.calendar_check = true;
        else if (argument == "--demo") options.demo = true;
        else if (argument == "--no-calendar") options.no_calendar = true;
        else throw std::runtime_error("Unknown option: " + notifier::plain_text(argument));
    }
    return options;
}

std::string calendar_content(const Options& options) {
    if (options.calendar_file.empty()) return notifier::download_calendar(options.calendar_url);
    std::ifstream input(options.calendar_file, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open calendar file");
    std::string content;
    std::array<char, 8192> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount()) {
        content.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
        if (content.size() > 8 * 1024 * 1024) throw std::runtime_error("Calendar exceeds 8 MiB");
    }
    return content;
}

std::vector<notifier::Event> load_calendar(const Options& options) {
    const auto now = std::time(nullptr);
    return notifier::parse_calendar(calendar_content(options), now, now + options.calendar_days * 86400);
}

std::string date(std::time_t timestamp, const char* format) {
    std::tm local{};
    localtime_r(&timestamp, &local);
    std::array<char, 128> buffer{};
    std::strftime(buffer.data(), buffer.size(), format, &local);
    return buffer.data();
}

struct CalendarSnapshot {
    std::vector<notifier::Event> events;
    std::string status = "Loading calendar...";
    bool stale = false;
    bool loaded = false;
};

class CalendarWorker {
public:
    explicit CalendarWorker(Options options) : options_(std::move(options)) {
        if (options_.demo) {
            const auto now = std::time(nullptr);
            snapshot_.events = {{now - 900, now + 900, false, "Project sync", "Meeting room 3"},
                                {now + 600, now + 4200, false, "Design review", "Microsoft Teams"}};
            snapshot_.status = "Offline demo";
            snapshot_.loaded = true;
        } else if (options_.no_calendar) snapshot_.status = "Calendar disabled";
        else if (options_.calendar_url.empty() && options_.calendar_file.empty()) snapshot_.status = "No calendar configured";
        else thread_ = std::thread([this] { run(); });
    }

    ~CalendarWorker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        condition_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

    CalendarSnapshot snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return snapshot_;
    }

    void refresh() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            refresh_ = true;
        }
        condition_.notify_one();
    }

private:
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_) {
            refresh_ = false;
            lock.unlock();
            CalendarSnapshot result;
            try {
                result.events = load_calendar(options_);
                result.loaded = true;
                result.status = "Updated " + date(std::time(nullptr), "%H:%M") + " | next " +
                                std::to_string(options_.calendar_days) + " days";
            } catch (const std::exception& error) {
                result.status = notifier::plain_text(error.what());
                result.stale = true;
            }
            lock.lock();
            if (result.stale) {
                result.events = snapshot_.events;
                result.loaded = snapshot_.loaded;
            }
            snapshot_ = std::move(result);
            condition_.wait_for(lock, std::chrono::seconds(options_.refresh_seconds),
                                [this] { return stop_ || refresh_; });
        }
    }

    Options options_;
    std::mutex mutex_;
    std::condition_variable condition_;
    CalendarSnapshot snapshot_;
    bool stop_ = false;
    bool refresh_ = false;
    std::thread thread_;
};

struct Line {
    std::string text;
    bool emphasis = false;
    int color = 0;
    bool highlighted = false;
};

std::vector<std::string> wrap(const std::string& raw, int columns) {
    const auto text = notifier::plain_text(raw);
    std::vector<std::string> lines;
    std::string line;
    int width = 0;
    std::mbstate_t state{};
    for (std::size_t position = 0; position < text.size();) {
        wchar_t character = 0;
        auto bytes = std::mbrtowc(&character, text.data() + position, text.size() - position, &state);
        if (bytes == static_cast<std::size_t>(-1) || bytes == static_cast<std::size_t>(-2)) {
            state = {};
            character = L'?';
            bytes = 1;
        }
        const auto measured = wcwidth(character);
        const int cells = measured < 0 ? 1 : measured;
        if (width + cells > columns && !line.empty()) {
            lines.push_back(std::move(line));
            line.clear();
            width = 0;
        }
        if (measured < 0 || character == L'?') line += '?';
        else line.append(text, position, bytes);
        width += cells;
        position += bytes;
    }
    if (!line.empty() || lines.empty()) lines.push_back(std::move(line));
    return lines;
}

void append(std::vector<Line>& lines, const std::string& text, int width, bool emphasis = false,
            int color = 0, bool highlighted = false) {
    for (auto& part : wrap(text, width)) lines.push_back({std::move(part), emphasis, color, highlighted});
}

void print_line(int row, int column, const std::string& text, int width) {
    if (width <= 0) return;
    const auto parts = wrap(text, width);
    mvaddnstr(row, column, parts.front().c_str(), static_cast<int>(parts.front().size()));
}

std::vector<Line> notification_lines(const std::deque<notifier::Notification>& notifications, int width) {
    std::vector<Line> lines;
    if (notifications.empty()) append(lines, "No notifications received yet.", width);
    for (const auto& notification : notifications) {
        const auto received = std::chrono::system_clock::to_time_t(notification.received);
        append(lines, date(received, "%a %H:%M") + "  " + notification.app, width, false, 4);
        append(lines, notification.summary.empty() ? "Notification" : notification.summary, width, true);
        if (!notification.body.empty()) append(lines, notification.body, width);
        lines.push_back({"", false, 0});
    }
    return lines;
}

std::vector<Line> calendar_lines(const CalendarSnapshot& calendar, int width) {
    std::vector<Line> lines;
    append(lines, calendar.status, width, false, calendar.stale ? 5 : 4);
    if (calendar.stale && !calendar.events.empty()) append(lines, "Showing last successful calendar", width, true, 5);
    lines.push_back({"", false, 0});
    std::string previous_day;
    const auto now = std::time(nullptr);
    const auto states = notifier::event_states(calendar.events, now);
    for (std::size_t index = 0; index < calendar.events.size(); ++index) {
        const auto& event = calendar.events[index];
        const auto state = states[index];
        if (state == notifier::EventState::past) continue;
        const auto day = date(event.start, "%a %d %b %Y");
        if (day != previous_day) {
            append(lines, day, width, true, 3);
            previous_day = day;
        }
        std::string timing = "All day";
        if (!event.all_day) {
            timing = date(event.start, "%H:%M") + "-" + date(event.end,
                date(event.start, "%F") == date(event.end, "%F") ? "%H:%M" : "%d %b %H:%M");
        }
        const bool ongoing = state == notifier::EventState::ongoing;
        const bool next = state == notifier::EventState::next;
        const bool highlighted = ongoing || next;
        const int color = ongoing ? 2 : next ? 3 : 4;
        if (ongoing) timing = "ONGOING  " + timing;
        else if (next) {
            const auto minutes = (event.start - now + 59) / 60;
            std::string countdown = std::to_string(minutes) + "m";
            if (minutes >= 1440) {
                countdown = std::to_string(minutes / 1440) + "d " + std::to_string(minutes % 1440 / 60) + "h";
            } else if (minutes >= 60) {
                countdown = std::to_string(minutes / 60) + "h " + std::to_string(minutes % 60) + "m";
            }
            timing = "NEXT in " + countdown + "  " + timing;
        }
        append(lines, timing, width, highlighted, color, highlighted);
        append(lines, event.title.empty() ? "Busy" : event.title, width, true, color, highlighted);
        if (!event.location.empty()) append(lines, event.location, width, false, highlighted ? color : 0);
        lines.push_back({"", false, 0});
    }
    if (lines.size() <= 2 && !calendar.stale) append(lines, "No upcoming events in this window.", width);
    return lines;
}

class Screen {
public:
    Screen() {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            throw std::runtime_error("An interactive terminal is required (or use --check-calendar)");
        }
        if (!initscr()) throw std::runtime_error("Could not initialize the terminal");
        cbreak();
        noecho();
        keypad(stdscr, true);
        curs_set(0);
        timeout(100);
        if (has_colors()) {
            start_color();
            use_default_colors();
            init_pair(1, COLOR_CYAN, -1);
            init_pair(2, COLOR_GREEN, -1);
            init_pair(3, COLOR_YELLOW, -1);
            init_pair(4, COLOR_WHITE, -1);
            init_pair(5, COLOR_RED, -1);
            init_pair(6, COLOR_WHITE, COLOR_RED);
        }
    }
    ~Screen() { endwin(); }
};

void inspect(const Options& options) {
    notifier::NotificationMonitor monitor;
    std::cout << "Watching notification metadata; Ctrl+C to stop.\n";
    while (!interrupted) {
        for (const auto& notification : monitor.poll(200)) {
            const auto lane = notifier::classify(notification, options.teams_patterns, options.outlook_patterns);
            std::cout << "app=" << std::quoted(notification.app)
                      << " desktop-entry=" << std::quoted(notification.desktop_entry)
                      << " origin=" << std::quoted(notifier::notification_origin(notification))
                      << " lane=" << (lane == notifier::Lane::teams ? "teams" :
                                     lane == notifier::Lane::outlook ? "outlook" : "unmatched") << std::endl;
        }
    }
}

void dashboard(const Options& options) {
    std::array<std::deque<notifier::Notification>, 2> histories;
    std::unique_ptr<notifier::NotificationMonitor> monitor;
    std::string monitor_status;
    std::size_t unmatched = 0;
    notifier::AttentionAlert alert;
    notifier::CalendarActivityTracker calendar_activity;
    if (options.demo) {
        notifier::Notification message;
        message.app = "Microsoft Teams";
        message.summary = "Engineering / Project channel";
        message.body = "The review notes are ready. Can we discuss them at the next sync?";
        notifier::remember(histories[0], message);
        message.app = "Outlook";
        message.summary = "Alex Morgan - Updated project schedule";
        message.body = "The updated milestones and meeting agenda are attached.";
        notifier::remember(histories[1], message);
        monitor_status = "DEMO - sample notifications";
        alert.trigger(notifier::AttentionAlert::Clock::now());
    } else {
        try {
            monitor = std::make_unique<notifier::NotificationMonitor>();
            monitor_status = "Desktop notifications connected";
        } catch (const std::exception& error) { monitor_status = error.what(); }
    }
    CalendarWorker calendar(options);
    Screen screen;
    int selected = 0;
    std::array<std::size_t, 3> offsets{};
    std::array<std::size_t, 3> totals{};
    while (!interrupted) {
        if (monitor) {
            try {
                for (auto& notification : monitor->poll()) {
                    const auto lane = notifier::classify(notification, options.teams_patterns, options.outlook_patterns);
                    if (lane == notifier::Lane::other) { ++unmatched; continue; }
                    const auto pane = lane == notifier::Lane::teams ? 0 : 1;
                    notifier::remember(histories[pane], std::move(notification));
                    alert.trigger(notifier::AttentionAlert::Clock::now());
                }
            } catch (const std::exception& error) {
                monitor_status = error.what();
                monitor.reset();
            }
        }
        const auto snapshot = calendar.snapshot();
        if (snapshot.loaded && calendar_activity.update(snapshot.events, std::time(nullptr))) {
            alert.trigger(notifier::AttentionAlert::Clock::now());
        }
        int rows = 0;
        int columns = 0;
        getmaxyx(stdscr, rows, columns);
        erase();
        const int height = std::max(1, rows - 6);
        if (rows < 12 || columns < 72) {
            print_line(0, 0, "Terminal Notifier: resize to at least 72 columns x 12 rows, q quits.", columns - 1);
        } else {
            attron(A_BOLD);
            print_line(0, 1, "TERMINAL NOTIFIER", columns - 24);
            print_line(0, columns - 20, date(std::time(nullptr), "%a %d %b %H:%M"), 19);
            attroff(A_BOLD);
            const std::array<std::string, 3> titles = {"TEAMS", "EMAIL / OUTLOOK", "CALENDAR"};
            for (int pane = 0; pane < 3; ++pane) {
                const int left = pane * columns / 3;
                const int right = (pane + 1) * columns / 3;
                const int width = right - left - 3;
                if (pane != 0) mvvline(1, left, ACS_VLINE, rows - 3);
                attron(COLOR_PAIR(pane + 1) | A_BOLD | (pane == selected ? A_REVERSE : 0));
                print_line(2, left + 1, " " + std::to_string(pane + 1) + " " + titles[pane] + " ", width);
                attroff(COLOR_PAIR(pane + 1) | A_BOLD | (pane == selected ? A_REVERSE : 0));
                const auto lines = pane < 2 ? notification_lines(histories[pane], width) : calendar_lines(snapshot, width);
                totals[pane] = lines.size();
                const auto maximum = lines.size() > static_cast<std::size_t>(height) ? lines.size() - height : 0;
                offsets[pane] = std::min(offsets[pane], maximum);
                for (int row = 0; row < height && offsets[pane] + row < lines.size(); ++row) {
                    const auto& line = lines[offsets[pane] + row];
                    const auto attributes = COLOR_PAIR(line.color) | (line.emphasis ? A_BOLD : 0) |
                                            (line.highlighted ? A_REVERSE : 0);
                    attron(attributes);
                    print_line(row + 4, left + 1, line.text, width);
                    attroff(attributes);
                }
            }
            print_line(rows - 2, 1, monitor_status + " | unmatched: " + std::to_string(unmatched), columns - 2);
            print_line(rows - 1, 1, "Tab/1-3 pane  Arrows/PgUp/PgDn scroll  r refresh  c clear  a ack  q quit", columns - 2);
        }
        if (rows > 0 && columns > 0 && alert.visible(notifier::AttentionAlert::Clock::now())) {
            const auto attributes = A_BOLD | (has_colors() ? COLOR_PAIR(6) : A_REVERSE);
            attron(attributes);
            mvhline(rows - 1, 0, ' ', columns);
            print_line(rows - 1, 1, "NEW ACTIVITY - press a to acknowledge", columns - 2);
            attroff(attributes);
        }
        refresh();
        const auto key = getch();
        auto& offset = offsets[selected];
        if (key == 'q' || key == 'Q') break;
        if (key == '\t') selected = (selected + 1) % 3;
        else if (key == KEY_BTAB) selected = (selected + 2) % 3;
        else if (key >= '1' && key <= '3') selected = key - '1';
        else if (key == KEY_DOWN || key == 'j') ++offset;
        else if ((key == KEY_UP || key == 'k') && offset) --offset;
        else if (key == KEY_NPAGE) offset += height;
        else if (key == KEY_PPAGE) offset = offset > static_cast<std::size_t>(height) ? offset - height : 0;
        else if (key == KEY_HOME) offset = 0;
        else if (key == KEY_END) offset = totals[selected];
        else if (key == 'r') calendar.refresh();
        else if (key == 'a' || key == 'A') alert.acknowledge();
        else if (key == 'c' && selected < 2) { histories[selected].clear(); offset = 0; }
    }
}

}

int main(int argc, char** argv) {
    std::setlocale(LC_ALL, "");
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
    try {
        const auto options = arguments(argc, argv);
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("Could not initialize HTTPS");
        struct CurlCleanup { ~CurlCleanup() { curl_global_cleanup(); } } cleanup;
        if (options.calendar_check) {
            const auto events = load_calendar(options);
            std::cout << "Calendar OK: " << events.size() << " upcoming events in " << options.calendar_days << " days\n";
        } else if (options.inspect) inspect(options);
        else dashboard(options);
    } catch (const std::exception& error) {
        std::cerr << "terminal-notifier: " << notifier::plain_text(error.what()) << '\n';
        return 1;
    }
}