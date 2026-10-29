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
#include <stdexcept>
#include <string>

namespace {

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string read_until(int descriptor, const std::string& expected) {
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd event{descriptor, POLLIN, 0};
        if (poll(&event, 1, 200) <= 0) continue;
        std::array<char, 8192> buffer{};
        const auto bytes = read(descriptor, buffer.data(), buffer.size());
        if (bytes <= 0) break;
        output.append(buffer.data(), static_cast<std::size_t>(bytes));
        if (output.find(expected) != std::string::npos) return output;
    }
    throw std::runtime_error("Expected terminal content was not rendered: " + expected);
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
    if (columns >= 72) {
        const auto output = read_until(descriptor, "NEW ACTIVITY");
        check(output.find("\033[41m") != std::string::npos, "Alert bar red background missing");
        check(output.find("TEAMS") != std::string::npos, "Teams heading missing");
        check(output.find("EMAIL / OUTLOOK") != std::string::npos, "Email heading missing");
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
        const auto output = read_until(descriptor, "NEW ACTIVITY");
        check(output.find("resize to at least") != std::string::npos, "Small-terminal resize message missing");
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
    exercise(argv[1], 40, 8);
    std::cout << "Terminal pane, resize and keyboard tests passed\n";
}