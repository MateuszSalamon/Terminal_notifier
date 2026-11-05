#include <pty.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string read_until(int descriptor, const std::string& expected, const std::string& also = {}) {
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd event{descriptor, POLLIN, 0};
        if (poll(&event, 1, 200) <= 0) continue;
        std::array<char, 8192> buffer{};
        const auto bytes = read(descriptor, buffer.data(), buffer.size());
        if (bytes <= 0) break;
        output.append(buffer.data(), static_cast<std::size_t>(bytes));
        if (output.find(expected) != std::string::npos &&
            (also.empty() || output.find(also) != std::string::npos)) return output;
    }
    throw std::runtime_error("Expected terminal content was not rendered: " + expected);
}

int rendered_row(const std::string& output, const std::string& text) {
    const auto position = output.find(text);
    check(position != std::string::npos, "Expected text missing from terminal output");
    const auto prefix = output.substr(0, position);
    const std::regex movement(R"(\x1b\[([0-9;]*)([A-Za-z])|(\n))");
    int row = 1;
    for (auto match = std::sregex_iterator(prefix.begin(), prefix.end(), movement);
         match != std::sregex_iterator{}; ++match) {
        if ((*match)[3].matched) { ++row; continue; }
        const auto parameter = (*match)[1].str();
        const int amount = parameter.empty() || parameter.front() == ';' ? 1 : std::stoi(parameter);
        const auto command = (*match)[2].str();
        if (command == "H" || command == "f" || command == "d") row = amount;
        else if (command == "A" || command == "F") row -= amount;
        else if (command == "B" || command == "E") row += amount;
    }
    return row;
}

void check_vertical(const std::string& raw_output, unsigned short rows) {
    const auto redraw = raw_output.rfind("\033[H");
    const auto output = redraw == std::string::npos ? raw_output : raw_output.substr(redraw);
    check(rendered_row(output, "TEAMS") == 3, "Teams must be on top");
    check(rendered_row(output, "EMAIL") == 3 + (rows - 4) / 3, "Email must be in the middle");
    check(rendered_row(output, "CALENDAR") == 3 + 2 * (rows - 4) / 3, "Calendar must be at the bottom");
    check(output.find("Engineering / Project channel") != std::string::npos,
        "Vertical Teams pane must display notification text");
    check(output.find("Alex Morgan") != std::string::npos, "Vertical email pane must display notification text");
    check(output.find("Project sync") != std::string::npos, "Vertical calendar must display event titles");
    check(output.find("ONGOING") != std::string::npos, "Vertical calendar must display event status");
    check(output.find("###") == std::string::npos && output.find("Resize to") == std::string::npos,
        "Vertical layout must take priority over compact layout");
    check(output.find("\033[41m") != std::string::npos, "Vertical alert bar red background missing");
}

void exercise(const char* executable, unsigned short columns, unsigned short rows) {
    winsize size{rows, columns, 0, 0};
    int descriptor = -1;
    const pid_t child = forkpty(&descriptor, nullptr, nullptr, &size);
    check(child >= 0, "Could not create a pseudo-terminal");
    if (child == 0) {
        setenv("TERM", "xterm-256color", 1);
        execl(executable, executable, "--demo", static_cast<char*>(nullptr));
        _exit(127);
    }
    struct ChildCleanup {
        pid_t child;
        int descriptor;
        bool reaped = false;
        ~ChildCleanup() {
            close(descriptor);
            if (!reaped) { kill(child, SIGKILL); waitpid(child, nullptr, 0); }
        }
    } cleanup{child, descriptor};
    if (columns < 50 && rows > 32) {
        check_vertical(read_until(descriptor, "NEW ACTIVITY"), rows);
    } else if (columns >= 72 && rows >= 12) {
        const auto output = read_until(descriptor, "NEW ACTIVITY");
        check(output.find("\033[41m") != std::string::npos, "Alert bar red background missing");
        check(output.find("TEAMS") != std::string::npos, "Teams heading missing");
        check(output.find("EMAIL") != std::string::npos, "Email heading missing");
        check(output.find("CALENDAR") != std::string::npos, "Calendar heading missing");
        check(output.find("Project sync") != std::string::npos, "Calendar event missing");
        check(output.find("ONGOING") != std::string::npos, "Current event marker missing");
        if (rows >= 30) {
            check(output.find("NEXT in ") != std::string::npos, "Next event countdown missing");
            check(output.find("Design review") != std::string::npos, "Next event title missing");
            check(output.find("\033[32mONGOING") != std::string::npos, "Current event green highlight missing");
            check(output.find("\033[33mNEXT") != std::string::npos, "Next event yellow highlight missing");
        }
    } else {
        const std::string notice = "Resize to 72 c wide and 12 r tall !";
        const auto last_line = notice.substr((notice.size() - 1) / columns * columns);
        const auto output = read_until(descriptor, last_line, "\033[41m");
        check(output.find("Resize to") != std::string::npos, "Compact resize notice missing");
        check(output.find("###") != std::string::npos && output.find("\033[31m") != std::string::npos,
            "Compact notification red ASCII blocks missing");
        const std::regex time_pattern(R"(\d{2}:\d{2})");
        check(std::distance(std::sregex_iterator(output.begin(), output.end(), time_pattern),
                            std::sregex_iterator{}) >= 4,
            "Compact panes must display both notification timestamps and both calendar event times");
        check(output.find("Project sync") == std::string::npos && output.find("Design review") == std::string::npos,
            "Compact calendar must hide event titles");
        check(output.find("Engineering") == std::string::npos && output.find("milestones") == std::string::npos,
            "Compact notifications must hide message text");
        check(output.find("NEXT in") == std::string::npos && output.find("ONGOING") == std::string::npos,
            "Compact calendar must display times without status labels");
        check(output.find("Tab/1-3 pane") == std::string::npos && output.find("NEW ACTIVITY") == std::string::npos,
            "Compact footer must hide keyboard and activity text");
    }
    if (!(columns < 50 && rows > 32)) {
        size.ws_col = 40;
        size.ws_row = 40;
        check(ioctl(descriptor, TIOCSWINSZ, &size) == 0, "Vertical terminal resize failed");
        kill(child, SIGWINCH);
        check(write(descriptor, "\f", 1) == 1, "Vertical terminal input failed");
        check_vertical(read_until(descriptor, "NEW ACTIVITY"), size.ws_row);
    }
    size.ws_col = 120;
    size.ws_row = 31;
    check(ioctl(descriptor, TIOCSWINSZ, &size) == 0, "Terminal resize failed");
    kill(child, SIGWINCH);
    check(write(descriptor, "\f", 1) == 1, "Terminal input failed");
    read_until(descriptor, "NEW ACTIVITY");
    check(write(descriptor, "a", 1) == 1, "Acknowledgement input failed");
    read_until(descriptor, "Tab/1-3 pane");
    const std::string keys = "2\033OB\033[6~3r\tcq";
    check(write(descriptor, keys.data(), keys.size()) == static_cast<ssize_t>(keys.size()), "Keyboard input failed");
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            cleanup.reaped = true;
            check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "Dashboard did not exit cleanly");
            return;
        }
        pollfd event{descriptor, POLLIN, 0};
        if (poll(&event, 1, 100) > 0) {
            std::array<char, 8192> buffer{};
            if (read(descriptor, buffer.data(), buffer.size()) < 0 && errno != EIO) break;
        }
    }
    throw std::runtime_error("Dashboard did not respond to quit");
}

}

int main(int argc, char** argv) {
    check(argc == 2, "Provide dashboard executable");
    exercise(argv[1], 120, 30);
    exercise(argv[1], 72, 12);
    exercise(argv[1], 71, 12);
    exercise(argv[1], 40, 8);
    exercise(argv[1], 72, 8);
    exercise(argv[1], 30, 6);
    exercise(argv[1], 18, 5);
    exercise(argv[1], 49, 33);
    exercise(argv[1], 40, 40);
    exercise(argv[1], 50, 40);
    exercise(argv[1], 49, 32);
    std::cout << "Terminal pane, resize and keyboard tests passed\n";
}