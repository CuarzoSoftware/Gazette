#ifndef SERVER_H
#define SERVER_H

#include <Core/Notification.h>
#include <CZ/Core/CZObject.h>
#include <memory>
#include <string>

namespace CZ { class CZBus; }
struct sd_bus_slot;

/**
 * @brief Gazette's D-Bus interfaces (user bus).
 *
 * - org.freedesktop.Notifications at /org/freedesktop/Notifications (the Desktop Notifications
 *   Specification 1.2): Notify, CloseNotification, GetCapabilities, GetServerInformation, and the
 *   NotificationClosed / ActionInvoked signals. Bodies are shown as plain text: their markup
 *   (body-markup) is removed.
 * - org.cuarzo.Gazette at /org/cuarzo/Gazette (used by Firmament's notifications button):
 *   - ToggleCenter(s screen), ShowCenter(s screen), HideCenter(): the notification center, on the
 *     screen with that name (the default one if empty or not found).
 *   - SetDoNotDisturb(b enabled)
 *   - GetState() → (b centerShown, b doNotDisturb, u count)
 *   - StateChanged(b centerShown, b doNotDisturb, u count) signal, emitted when any changes.
 */
class Server final : public CZObject
{
public:
    Server() noexcept;
    ~Server() noexcept;

    void emitClosed(UInt32 id, Notification::CloseReason reason) noexcept;
    void emitActionInvoked(UInt32 id, const std::string &action) noexcept;

    /// Removes markup (tags) and decodes the entities of a body.
    static std::string PlainText(const std::string &markup) noexcept;

private:
    void emitState() noexcept;
    std::shared_ptr<CZBus> m_bus;
    sd_bus_slot *m_notificationsSlot {}, *m_gazetteSlot {};
};

#endif // SERVER_H
