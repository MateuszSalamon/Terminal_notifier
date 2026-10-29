#include "notifications.hpp"

#include <dbus/dbus.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace notifier {
namespace {

using Message = std::unique_ptr<DBusMessage, decltype(&dbus_message_unref)>;

std::string string_argument(DBusMessageIter& iterator) {
    const char* text = nullptr;
    dbus_message_iter_get_basic(&iterator, &text);
    dbus_message_iter_next(&iterator);
    return text ? plain_text(text) : "";
}

}

struct NotificationMonitor::Impl {
    DBusConnection* connection = nullptr;
    ~Impl() {
        if (connection) {
            dbus_connection_close(connection);
            dbus_connection_unref(connection);
        }
    }
};

NotificationMonitor::NotificationMonitor() : impl_(std::make_unique<Impl>()) {
    DBusError error;
    dbus_error_init(&error);
    impl_->connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (!impl_->connection) {
        const std::string reason = error.message ? error.message : "No session bus";
        dbus_error_free(&error);
        throw std::runtime_error("Notifications unavailable: " + plain_text(reason));
    }
    dbus_connection_set_exit_on_disconnect(impl_->connection, false);
    Message request(dbus_message_new_method_call(DBUS_SERVICE_DBUS, DBUS_PATH_DBUS,
        "org.freedesktop.DBus.Monitoring", "BecomeMonitor"), dbus_message_unref);
    if (!request) throw std::runtime_error("Could not allocate notification monitor request");
    DBusMessageIter arguments;
    DBusMessageIter rules;
    dbus_message_iter_init_append(request.get(), &arguments);
    const char* rule = "type='method_call',interface='org.freedesktop.Notifications',member='Notify'";
    const dbus_uint32_t flags = 0;
    if (!dbus_message_iter_open_container(&arguments, DBUS_TYPE_ARRAY, "s", &rules) ||
        !dbus_message_iter_append_basic(&rules, DBUS_TYPE_STRING, &rule) ||
        !dbus_message_iter_close_container(&arguments, &rules) ||
        !dbus_message_iter_append_basic(&arguments, DBUS_TYPE_UINT32, &flags)) {
        throw std::runtime_error("Could not allocate notification monitor arguments");
    }
    Message reply(dbus_connection_send_with_reply_and_block(impl_->connection, request.get(),
        3000, &error), dbus_message_unref);
    if (!reply) {
        const std::string reason = error.message ? error.message : "Monitoring denied";
        dbus_error_free(&error);
        throw std::runtime_error("Notification monitoring denied: " + plain_text(reason));
    }
}

NotificationMonitor::~NotificationMonitor() = default;

std::vector<Notification> NotificationMonitor::poll(int timeout_ms) {
    if (!dbus_connection_read_write(impl_->connection, timeout_ms)) {
        throw std::runtime_error("Desktop notification bus disconnected; restart to reconnect");
    }
    std::vector<Notification> notifications;
    for (int index = 0; index < 64; ++index) {
        Message message(dbus_connection_pop_message(impl_->connection), dbus_message_unref);
        if (!message) break;
        if (!dbus_message_is_method_call(message.get(), "org.freedesktop.Notifications", "Notify") ||
            !dbus_message_has_signature(message.get(), "susssasa{sv}i")) continue;
        DBusMessageIter arguments;
        dbus_message_iter_init(message.get(), &arguments);
        Notification notification;
        notification.app = string_argument(arguments);
        dbus_message_iter_get_basic(&arguments, &notification.replaces_id);
        dbus_message_iter_next(&arguments);
        string_argument(arguments);
        notification.summary = string_argument(arguments);
        notification.body = string_argument(arguments);
        dbus_message_iter_next(&arguments);
        DBusMessageIter hints;
        dbus_message_iter_recurse(&arguments, &hints);
        while (dbus_message_iter_get_arg_type(&hints) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry;
            dbus_message_iter_recurse(&hints, &entry);
            const auto key = string_argument(entry);
            DBusMessageIter variant;
            dbus_message_iter_recurse(&entry, &variant);
            if (dbus_message_iter_get_arg_type(&variant) == DBUS_TYPE_STRING) {
                if (key == "desktop-entry") notification.desktop_entry = string_argument(variant);
                else if (key == "x-kde-origin-name") notification.origin = string_argument(variant);
            }
            dbus_message_iter_next(&hints);
        }
        const auto* sender = dbus_message_get_sender(message.get());
        notification.sender = sender ? sender : "";
        notifications.push_back(std::move(notification));
    }
    return notifications;
}

}