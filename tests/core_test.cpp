#include "core.hpp"

#include <iostream>
#include <stdexcept>

void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    using namespace notifier;
    Notification notification;
    notification.app = "Microsoft Teams";
    check(classify(notification) == Lane::teams, "Teams app routing");
    notification.app = "Google Chrome";
    notification.desktop_entry = "chrome-outlook-profile1";
    check(classify(notification) == Lane::outlook, "Outlook PWA routing");
    notification.desktop_entry = "chrome-opaqueid-profile1";
    check(classify(notification) == Lane::other, "Unknown browser is not guessed");
    check(classify(notification, {"OPAQUEID"}) == Lane::teams, "Custom PWA matching");
    notification.desktop_entry.clear();
    notification.summary = "Outlook: new mail";
    check(classify(notification) == Lane::outlook, "Notification summary routing");
    Notification chromium;
    chromium.app = "Chromium";
    chromium.desktop_entry = "chromium_chromium";
    chromium.summary = "Alex - Updated schedule";
    chromium.body = "outlook.office.com\n\nThe schedule is attached.";
    check(classify(chromium) == Lane::outlook, "Chromium Outlook origin in notification body");
    chromium.summary = "Teams meeting notes";
    check(classify(chromium) == Lane::outlook, "Website origin takes precedence over subject keywords");
    chromium.body = "<a href=\"https://outlook.office.com/\">outlook.office.com</a>\n\nNew email";
    check(classify(chromium) == Lane::outlook, "Chromium hyperlink-formatted website origin");
    chromium.summary = "Alex - Updated schedule";
    chromium.body = "teams.microsoft.com\n\nNew chat message";
    check(classify(chromium) == Lane::teams, "Chromium Teams website origin");
    chromium.body = "example.com\n\nA message discussing Outlook and Teams";
    check(classify(chromium) == Lane::other, "Message text is not treated as an app identifier");
    chromium.body = "outlook.office.com.example.org\n\nUnrelated website";
    check(classify(chromium) == Lane::other, "Lookalike website is not Outlook");
    chromium.body = "New email with no domain in its contents";
    chromium.origin = "https://OUTLOOK.OFFICE.COM/";
    check(notification_origin(chromium) == "outlook.office.com" && classify(chromium) == Lane::outlook,
        "Explicit Chromium origin hint is case insensitive");
    chromium.origin = "teams.microsoft.com";
    check(classify(chromium) == Lane::teams, "Teams origin hint");
    chromium.origin = "outlook.office.com.example.org";
    check(classify(chromium) == Lane::other, "Lookalike origin hint does not fall back to keyword routing");
    chromium.origin = "example.com";
    chromium.body = "outlook.office.com\n\nQuoted website";
    check(notification_origin(chromium).empty() && classify(chromium) == Lane::other,
        "An explicit unrelated origin is not overridden by message text");
    chromium.origin.clear();
    chromium.body = "outlook.office.com@unrelated.example\n\nUnrelated website";
    check(classify(chromium) == Lane::other, "URL user information is not a trusted Outlook hostname");
    chromium.body = "example.com\n\nSee https://outlook.office.com/ for details";
    check(classify(chromium) == Lane::other, "Only leading browser context is considered");
    chromium.body = plain_text("outlook.office365.com\n\nNew email");
    check(classify(chromium) == Lane::outlook, "Sanitized Chromium context remains routable");
    check(plain_text("hello\nworld\x1b\a") == "hello world", "Control stripping");
    check(plain_text(std::string(20000, 'a')).size() == 16384, "Text limit");
    std::deque<Notification> history;
    notification.sender = ":1.1";
    notification.replaces_id = 42;
    remember(history, notification);
    notification.body = "updated";
    remember(history, notification);
    check(history.size() == 1 && history.front().body == "updated", "Replacement update");
    notification.sender = ":1.2";
    remember(history, notification);
    check(history.size() == 2, "Replacement IDs are scoped to sender");
    notification.replaces_id = 0;
    for (int index = 0; index < 250; ++index) remember(history, notification);
    check(history.size() == 200, "Bounded history");
    AttentionAlert alert;
    const AttentionAlert::Clock::time_point start{};
    using namespace std::chrono_literals;
    check(!alert.pending() && !alert.visible(start), "No alert without activity");
    alert.trigger(start);
    check(alert.pending() && alert.visible(start) && alert.visible(start + 4999ms), "Alert starts with five seconds on");
    check(!alert.visible(start + 5s) && !alert.visible(start + 9999ms), "Five seconds off at the exact boundary");
    check(alert.visible(start + 10s) && !alert.visible(start + 15s) && alert.visible(start + 20s),
        "Flashing repeats until acknowledged");
    alert.trigger(start + 17s);
    check(!alert.visible(start + 17s) && alert.visible(start + 20s), "New activity preserves an existing flash cycle");
    alert.acknowledge();
    check(!alert.pending() && !alert.visible(start + 20s), "Acknowledgement stops flashing");
    alert.trigger(start + 22s);
    check(alert.visible(start + 22s) && !alert.visible(start + 27s), "Later activity re-arms a fresh cycle");
    alert.acknowledge();
    alert.acknowledge();
    check(!alert.pending(), "Acknowledgement is safe when no alert is pending");
    std::cout << "Notification routing tests passed\n";
}