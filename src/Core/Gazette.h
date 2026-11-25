#ifndef GAZETTE_H
#define GAZETTE_H

#include <Core/Notification.h>
#include <CZ/Services/SVNotificationsServer.h>
#include <CZ/Core/CZObject.h>
#include <CZ/Core/CZSignal.h>
#include <filesystem>
#include <list>
#include <memory>
#include <vector>

namespace CZ { class MApp; class MScreen; class XDGKit; class ABCore; class CZBus; }
struct sd_bus_slot;

class BannerSurface;
class CenterSurface;

/**
 * @brief The notifications daemon.
 *
 * - Keeps the notifications (newest first) until they're closed: by the user, by their app
 *   (CloseNotification), or by invoking one of their actions (unless resident).
 * - New ones are shown as banners at the top right of every screen (see BannerSurface), unless Do Not
 *   Disturb is on. Banners hide after a while (critical ones and those with expire_timeout 0 stay),
 *   the notification stays in the notification center.
 * - The notification center (see CenterSurface) slides from the right: Do Not Disturb and the
 *   list. Toggled by Firmament's notifications button (org.cuarzo.Gazette.ToggleCenter, see SVNotificationsServer).
 *
 * Do Not Disturb is saved to ~/.config/cz-gazette/gazette.conf. Opening Exposé or switching
 * workspaces (Crystals' ExposeChanged and WorkspaceChanged signals) hides the notification center.
 */
class Gazette final : public CZObject
{
public:
    Gazette() noexcept;
    ~Gazette() noexcept;

    std::shared_ptr<MApp> marco;        ///< The Wayland app
    std::shared_ptr<XDGKit> xdgkit;     ///< Icon themes and .desktop entries (apps' names and icons)
    std::shared_ptr<ABCore> ab;         ///< Icons, shared with the other apps when the asset broker runs
    std::shared_ptr<SVNotificationsServer> server; ///< The D-Bus side (see notify())
    std::vector<std::unique_ptr<BannerSurface>> banners; ///< The banners, one surface per screen
    std::unique_ptr<CenterSurface> center;   ///< The notification center

    /// Newest first.
    const std::list<std::shared_ptr<Notification>> &notifications() const noexcept { return m_notifications; }
    /// The notification with that id, nullptr if it's not there (anymore).
    std::shared_ptr<Notification> find(UInt32 id) const noexcept;

    /**
     * @brief A notification arrived (Notify, see SVNotificationsServer): prepares it for showing
     *        (plain text, the app's name, its icon and image) and adds it.
     * @return Its id.
     */
    UInt32 notify(const SVNotification &sent) noexcept;

    /**
     * @brief Adds @p n (or replaces the one with its id if it's still there, else it gets a new id).
     * @return Its id.
     */
    UInt32 add(std::shared_ptr<Notification> n, UInt32 replacesId) noexcept;

    /// Removes it (banner and center), and tells its app (NotificationClosed).
    void close(UInt32 id, Notification::CloseReason reason) noexcept;

    /// Tells its app (ActionInvoked), then closes it (unless resident).
    void invoke(UInt32 id, const std::string &action) noexcept;

    /// Its banner timed out: transient notifications are closed, the rest stay in the center.
    void bannerExpired(UInt32 id) noexcept;

    /// Its banner was dragged away on a screen: it leaves the other screens too, then see bannerExpired().
    void bannerDismissed(UInt32 id) noexcept;

    /// Closes every notification (Dismissed).
    void clearAll() noexcept;

    /// Do Not Disturb: new notifications go to the center only (no banners).
    bool doNotDisturb() const noexcept { return m_dnd; }

    /// Turns Do Not Disturb on or off (saved; turning it on hides the banners).
    void setDoNotDisturb(bool enabled) noexcept;

    /// Shows or hides the notification center on @p screen (by name, the default screen if not found).
    void toggleCenter(const std::string &screen = {}) noexcept;

    /// Shows the notification center on @p screen (by name, the default screen if not found).
    void showCenter(const std::string &screen = {}) noexcept;

    /// Hides the notification center (sliding out).
    void hideCenter() noexcept;

    /// The notification center is shown (or sliding in).
    bool centerShown() const noexcept;

    /// The screen at (0,0), else the first one.
    MScreen *defaultScreen() const noexcept;
    /// The screen named @p name (e.g. "eDP-1"), else defaultScreen().
    MScreen *screenNamed(const std::string &name) const noexcept;

    /// Loads the app icon of @p n (its app_icon, else its .desktop entry's, else a generic one).
    std::shared_ptr<RImage> loadAppIcon(const Notification &n) noexcept;

    /// Loads a themed icon by name.
    std::shared_ptr<RImage> loadIcon(const std::string &name, Int32 size) noexcept;

    /// Center shown, Do Not Disturb or the count changed (see SVNotificationsServer::emitCenterState()).
    CZSignal<> onStateChanged;

    /// A notification was added, replaced or removed.
    CZSignal<> onNotificationsChanged;

private:
    static std::filesystem::path SettingsPath() noexcept;
    void loadSettings() noexcept;
    void saveSettings() noexcept;
    void initExposeWatch() noexcept;
    void screenPlugged(MScreen &screen) noexcept;
    void screenUnplugged(MScreen &screen) noexcept;
    std::shared_ptr<CZBus> m_bus;
    sd_bus_slot *m_exposeMatch {}, *m_workspaceMatch {};
    std::list<std::shared_ptr<Notification>> m_notifications;
    UInt32 m_lastId { 0 };
    bool m_dnd { false };
};

/// The Gazette instance (made in main()).
Gazette *GetGazette() noexcept;

#endif // GAZETTE_H
