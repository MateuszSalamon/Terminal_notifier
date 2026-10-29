#include "core.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <utility>

namespace notifier {
namespace {

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return text;
}

bool matches(const std::string& source, const std::vector<std::string>& patterns) {
    return std::any_of(patterns.begin(), patterns.end(), [&](const std::string& pattern) {
        return !pattern.empty() && source.find(lowercase(pattern)) != std::string::npos;
    });
}

}

void AttentionAlert::trigger(Clock::time_point now) {
    if (!pending_) started_ = now;
    pending_ = true;
}

void AttentionAlert::acknowledge() {
    pending_ = false;
}

bool AttentionAlert::pending() const {
    return pending_;
}

bool AttentionAlert::visible(Clock::time_point now) const {
    return pending_ && std::chrono::duration_cast<std::chrono::seconds>(now - started_).count() % 10 < 5;
}

std::string plain_text(const std::string& text) {
    std::string result;
    for (unsigned char character : text) {
        if (result.size() >= 16384) break;
        if (character == '\n' || character == '\t') result += ' ';
        else if (character >= 32 && character != 127) result += static_cast<char>(character);
    }
    return result;
}

std::string notification_origin(const Notification& notification) {
    static const std::regex context(
        R"origin(^\s*(?:<a\s+href\s*=\s*["'])?(?:https?://)?(outlook\.office(?:365)?\.com|outlook\.live\.com|outlook\.cloud\.microsoft|teams\.microsoft\.com|teams\.live\.com|teams\.cloud\.microsoft)(?=[/?#\s"'<>]|:\d+(?:[/\s"'<>]|$)|$))origin",
        std::regex::icase);
    const auto browser = lowercase(notification.app + "\n" + notification.desktop_entry);
    if (notification.origin.empty() &&
        !matches(browser, {"chromium", "chrome", "firefox", "brave", "microsoft-edge", "vivaldi", "opera"})) return {};
    const auto& text = notification.origin.empty() ? notification.body : notification.origin;
    std::smatch match;
    return std::regex_search(text, match, context) ? lowercase(match[1].str()) : "";
}

Lane classify(const Notification& notification,
              const std::vector<std::string>& teams_patterns,
              const std::vector<std::string>& outlook_patterns) {
    const auto metadata = lowercase(notification.app + "\n" + notification.desktop_entry +
                                    "\n" + notification.summary);
    const auto source = metadata + "\n" + lowercase(notification.origin);
    if (matches(source, teams_patterns)) return Lane::teams;
    if (matches(source, outlook_patterns)) return Lane::outlook;
    const auto origin = notification_origin(notification);
    if (origin.rfind("outlook.", 0) == 0) return Lane::outlook;
    if (origin.rfind("teams.", 0) == 0) return Lane::teams;
    if (matches(metadata, {"teams", "msteams"})) return Lane::teams;
    if (matches(metadata, {"outlook"})) return Lane::outlook;
    return Lane::other;
}

void remember(std::deque<Notification>& history, Notification notification) {
    if (notification.replaces_id != 0) {
        const auto previous = std::find_if(history.begin(), history.end(), [&](const auto& item) {
            return item.sender == notification.sender &&
                   item.replaces_id == notification.replaces_id;
        });
        if (previous != history.end()) history.erase(previous);
    }
    history.push_front(std::move(notification));
    if (history.size() > 200) history.pop_back();
}

}