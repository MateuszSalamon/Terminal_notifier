#include "notifications.hpp"

#include <dbus/dbus.h>

#include <iostream>
#include <memory>
#include <stdexcept>

void verify_notification(DBusConnection* connection, notifier::NotificationMonitor& monitor,
                         const char* app, const char* desktop, const char* body,
                         const char* origin, notifier::Lane expected) {
    std::unique_ptr<DBusMessage, decltype(&dbus_message_unref)> message(
        dbus_message_new_method_call("org.freedesktop.Notifications", "/org/freedesktop/Notifications",
                                    "org.freedesktop.Notifications", "Notify"), dbus_message_unref);
    DBusMessageIter arguments;
    dbus_message_iter_init_append(message.get(), &arguments);
    const char* icon = "";
    const char* summary = "New message";
    const dbus_uint32_t replacement = 7;
    const dbus_int32_t timeout = -1;
    dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING, &app);
    dbus_message_iter_append_basic(&arguments, DBUS_TYPE_UINT32, &replacement);
    dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING, &icon);
    dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING, &summary);
    dbus_message_iter_append_basic(&arguments, DBUS_TYPE_STRING, &body);
    DBusMessageIter actions;
    dbus_message_iter_open_container(&arguments, DBUS_TYPE_ARRAY, "s", &actions);
    dbus_message_iter_close_container(&arguments, &actions);
    DBusMessageIter hints;
    dbus_message_iter_open_container(&arguments, DBUS_TYPE_ARRAY, "{sv}", &hints);
    auto add_hint = [&](const char* key, const char* value) {
        DBusMessageIter entry;
        DBusMessageIter variant;
        dbus_message_iter_open_container(&hints, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&hints, &entry);
    };
    add_hint("desktop-entry", desktop);
    if (*origin) add_hint("x-kde-origin-name", origin);
    dbus_message_iter_close_container(&arguments, &hints);
    dbus_message_iter_append_basic(&arguments, DBUS_TYPE_INT32, &timeout);
    dbus_message_set_no_reply(message.get(), true);
    dbus_connection_send(connection, message.get(), nullptr);
    dbus_connection_flush(connection);
    for (int attempt = 0; attempt < 10; ++attempt) {
        for (const auto& notification : monitor.poll(100)) {
            if (notification.summary != summary) continue;
            if (notification.app != app || notification.body != notifier::plain_text(body) ||
                notification.desktop_entry != desktop || notification.origin != origin ||
                notification.replaces_id != replacement || notification.sender.empty() ||
                notifier::classify(notification) != expected) {
                throw std::runtime_error("D-Bus notification decoding failed");
            }
            return;
        }
    }
    throw std::runtime_error("No monitored notification received");
}

int main() {
    DBusError error;
    dbus_error_init(&error);
    auto* connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (!connection) throw std::runtime_error("Test session bus unavailable");
    struct ConnectionCleanup {
        DBusConnection* connection;
        ~ConnectionCleanup() { dbus_connection_close(connection); dbus_connection_unref(connection); }
    } cleanup{connection};
    dbus_connection_set_exit_on_disconnect(connection, false);
    dbus_bus_request_name(connection, "org.freedesktop.Notifications", DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    notifier::NotificationMonitor monitor;
    verify_notification(connection, monitor, "Google Chrome", "chrome-msteams-test",
                        "Integration test message", "", notifier::Lane::teams);
    verify_notification(connection, monitor, "Chromium", "chromium_chromium",
                        "outlook.office.com\n\nEmail preview", "", notifier::Lane::outlook);
    verify_notification(connection, monitor, "Chromium", "chromium_chromium",
                        "<a href=\"https://outlook.office.com/\">outlook.office.com</a>\n\nEmail preview",
                        "", notifier::Lane::outlook);
    verify_notification(connection, monitor, "Chromium", "chromium_chromium",
                        "Email preview", "outlook.office.com", notifier::Lane::outlook);
    verify_notification(connection, monitor, "Chromium", "chromium_chromium",
                        "Chat preview", "teams.microsoft.com", notifier::Lane::teams);
    verify_notification(connection, monitor, "Chromium", "chromium_chromium",
                        "Other preview", "outlook.office.com.example.org", notifier::Lane::other);
    std::cout << "D-Bus notification integration tests passed\n";
}