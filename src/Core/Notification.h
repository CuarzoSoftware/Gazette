#ifndef NOTIFICATION_H
#define NOTIFICATION_H

#include <CZ/Core/Cuarzo.h>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace CZ { class RImage; }

using namespace CZ;

#define DEFAULT_ICON_THEME "WhiteSur-light"
#define FALLBACK_ICON_THEME "WhiteSur"

/**
 * @brief A notification sent through org.freedesktop.Notifications.Notify (see Server).
 */
struct Notification
{
    /// Why a notification was closed (NotificationClosed's reason)
    enum class CloseReason : UInt32
    {
        Expired = 1,    ///< Its banner timed out (transient notifications only, see Gazette::bannerExpired())
        Dismissed = 2,  ///< By the user (closed, cleared, or an action invoked)
        Closed = 3,     ///< By the app (CloseNotification)
        Undefined = 4
    };

    /// urgency hint
    enum class Urgency : UInt8 { Low = 0, Normal = 1, Critical = 2 };

    struct Action
    {
        std::string key;
        std::string label;
    };

    UInt32 id { 0 };
    std::string appName;        ///< The app's name (Notify's app_name, else its .desktop entry's Name)
    std::string appIcon;        ///< app_icon: an icon name, a path or a file:// URI
    std::string desktopEntry;   ///< desktop-entry hint (the app's .desktop file id)
    std::string summary;        ///< Plain text (single line)
    std::string body;           ///< Plain text (markup removed, see Server)
    std::vector<Action> actions;  ///< Buttons (the "default" one isn't here, see hasDefault)
    bool hasDefault { false };  ///< Clicking the notification invokes the "default" action
    Urgency urgency { Urgency::Normal };
    Int32 timeout { -1 };       ///< expire_timeout (ms): -1 the default, 0 never (the banner stays)
    bool transient { false };   ///< Never kept in the notification center
    bool resident { false };    ///< Not removed when an action is invoked
    std::shared_ptr<RImage> image; ///< image-data / image-path hint (shown at the right)
    std::shared_ptr<RImage> icon;  ///< The app's icon
    std::chrono::system_clock::time_point time; ///< When it was sent (or replaced)

    /// "now", "5m ago", "2h ago", else the date
    std::string timeText() const noexcept;
};

#endif // NOTIFICATION_H
