#ifndef NOTIFICATION_H
#define NOTIFICATION_H

#include <CZ/Services/SNotification.h>
#include <chrono>
#include <memory>
#include <string>

namespace CZ { class RImage; }

using namespace CZ;

/// The icon theme looked up first (apps' icons)
#define DEFAULT_ICON_THEME "WhiteSur-light"

/// Looked up when DEFAULT_ICON_THEME lacks an icon (WhiteSur-light doesn't inherit it)
#define FALLBACK_ICON_THEME "WhiteSur"

/**
 * @brief A notification as Gazette shows it (see Gazette::notify()).
 *
 * What was sent (SNotification, read by SNotificationsServer), with its summary and body as plain
 * text, the app's name from its .desktop entry if not given, and its icon and image loaded.
 */
struct Notification : public SNotification
{
    UInt32 id { 0 };                 ///< Never 0 (Notify's replaces_id uses it for "none")
    std::shared_ptr<RImage> picture; ///< The image (image-data / image-path hint), shown at the right
    std::shared_ptr<RImage> icon;    ///< The app's icon
    std::chrono::system_clock::time_point time; ///< When it was sent (or replaced)

    /// "now", "5m ago", "2h ago", else the date
    std::string timeText() const noexcept;
};

#endif // NOTIFICATION_H
