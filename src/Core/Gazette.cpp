#include <Core/Gazette.h>
#include <Core/Server.h>
#include <Core/Log.h>
#include <Surfaces/BannerSurface.h>
#include <Surfaces/CenterSurface.h>
#include <CZ/Marco/MApp.h>
#include <CZ/Marco/MScreen.h>
#include <CZ/XDG/XDGKit.h>
#include <CZ/XDG/XDGDesktopEntry.h>
#include <CZ/XDG/XDGINI.h>
#include <CZ/AssetBroker/ABCore.h>
#include <CZ/AssetBroker/ABIconKey.h>
#include <CZ/Ream/RCore.h>
#include <CZ/Ream/RDevice.h>
#include <CZ/Ream/RImage.h>
#include <CZ/Core/CZBus.h>
#include <systemd/sd-bus.h>
#include <drm_fourcc.h>
#include <algorithm>
#include <cctype>
#include <fstream>

namespace fs = std::filesystem;

static Gazette *_gazette;

Gazette *GetGazette() noexcept { return _gazette; }

Gazette::Gazette() noexcept
{
    _gazette = this;
    marco = MApp::Make();

    if (!marco)
        return;

    xdgkit = XDGKit::Make();

    // Icons through the cz-asset-broker service if it's running, else loaded here
    ab = ABCore::GetOrMake();

    if (ab)
        ab->setXDGKit(xdgkit);

    loadSettings();
    banners = std::make_unique<BannerSurface>();
    center = std::make_unique<CenterSurface>();
    server = std::make_unique<Server>();
    initExposeWatch();
}

void Gazette::initExposeWatch() noexcept
{
    m_bus = CZBus::GetOrMakeUser();

    if (!m_bus)
        return;

    // Exposé shown: the center hides (Exposé shows the windows, it'd be in the way)
    sd_bus_match_signal_async(m_bus->bus(), &m_exposeMatch, nullptr, "/org/cuarzo/Crystals", "org.cuarzo.Crystals", "ExposeChanged",
        [](sd_bus_message *m, void *, sd_bus_error *) -> int {
            int shown { 0 };
            const char *output {};

            if (sd_bus_message_read(m, "bs", &shown, &output) >= 0 && shown && GetGazette())
                GetGazette()->hideCenter();

            return 0;
        }, nullptr, nullptr);

    // Another workspace: the center hides
    sd_bus_match_signal_async(m_bus->bus(), &m_workspaceMatch, nullptr, "/org/cuarzo/Crystals", "org.cuarzo.Crystals", "WorkspaceChanged",
        [](sd_bus_message *, void *, sd_bus_error *) -> int {
            if (GetGazette())
                GetGazette()->hideCenter();

            return 0;
        }, nullptr, nullptr);
}

Gazette::~Gazette() noexcept
{
    sd_bus_slot_unref(m_exposeMatch);
    sd_bus_slot_unref(m_workspaceMatch);
    server.reset();
    center.reset();
    banners.reset();
    _gazette = nullptr;
}

/* -------------------------------------- Notifications -------------------------------------- */

std::shared_ptr<Notification> Gazette::find(UInt32 id) const noexcept
{
    for (const auto &n : m_notifications)
        if (n->id == id)
            return n;

    return {};
}

UInt32 Gazette::add(std::shared_ptr<Notification> n, UInt32 replacesId) noexcept
{
    n->time = std::chrono::system_clock::now();
    n->icon = loadAppIcon(*n);

    // Replacing one still there: same id and place in the list, its banner updated
    if (replacesId != 0)
    {
        for (auto &old : m_notifications)
        {
            if (old->id != replacesId)
                continue;

            n->id = replacesId;
            old = n;
            banners->replace(n);
            onNotificationsChanged.notify();
            onStateChanged.notify();
            return n->id;
        }
    }

    // Never 0 (it means "none" in Notify's replaces_id)
    if (++m_lastId == 0)
        m_lastId = 1;

    n->id = m_lastId;
    m_notifications.push_front(n);

    // Do Not Disturb: only to the center (critical ones too, like macOS's Focus by default)
    if (!m_dnd && !center->shown())
        banners->show(n);

    onNotificationsChanged.notify();
    onStateChanged.notify();
    Log(CZTrace, CZLN, "Notification {} from {}: {}", n->id, n->appName, n->summary);
    return n->id;
}

void Gazette::close(UInt32 id, Notification::CloseReason reason) noexcept
{
    const auto it { std::find_if(m_notifications.begin(), m_notifications.end(),
        [id](const auto &n){ return n->id == id; }) };

    if (it == m_notifications.end())
        return;

    m_notifications.erase(it);
    banners->remove(id);
    server->emitClosed(id, reason);
    onNotificationsChanged.notify();
    onStateChanged.notify();
}

void Gazette::invoke(UInt32 id, const std::string &action) noexcept
{
    const auto n { find(id) };

    if (!n)
        return;

    server->emitActionInvoked(id, action);

    if (!n->resident)
        close(id, Notification::CloseReason::Dismissed);
}

void Gazette::bannerExpired(UInt32 id) noexcept
{
    if (const auto n { find(id) }; n && n->transient)
        close(id, Notification::CloseReason::Expired);
}

void Gazette::clearAll() noexcept
{
    std::vector<UInt32> ids;

    for (const auto &n : m_notifications)
        ids.push_back(n->id);

    for (UInt32 id : ids)
        close(id, Notification::CloseReason::Dismissed);
}

void Gazette::setDoNotDisturb(bool enabled) noexcept
{
    if (m_dnd == enabled)
        return;

    m_dnd = enabled;
    saveSettings();

    if (m_dnd)
        banners->hideAll();

    onStateChanged.notify();
}

/* ------------------------------------------ Center ------------------------------------------ */

void Gazette::toggleCenter(const std::string &screen) noexcept
{
    if (center->shown())
        hideCenter();
    else
        showCenter(screen);
}

void Gazette::showCenter(const std::string &screen) noexcept
{
    if (center->shown())
        return;

    // The banners are in the list too
    banners->hideAll();
    center->show(screenNamed(screen));
    onStateChanged.notify();
}

void Gazette::hideCenter() noexcept
{
    if (!center->shown())
        return;

    center->hide();
    onStateChanged.notify();
}

bool Gazette::centerShown() const noexcept
{
    return center && center->shown();
}

MScreen *Gazette::defaultScreen() const noexcept
{
    if (!marco || marco->screens().empty())
        return nullptr;

    for (MScreen *s : marco->screens())
        if (s->props().logicalPos && *s->props().logicalPos == SkIPoint::Make(0, 0))
            return s;

    return marco->screens().front();
}

MScreen *Gazette::screenNamed(const std::string &name) const noexcept
{
    if (marco && !name.empty())
        for (MScreen *s : marco->screens())
            if (s->props().name == name)
                return s;

    return defaultScreen();
}

/* ------------------------------------------ Icons ------------------------------------------ */

std::shared_ptr<RImage> Gazette::loadIcon(const std::string &name, Int32 size) noexcept
{
    if (!ab || name.empty())
        return {};

    ABIconKey key;
    key.names = { name };
    key.size = size;
    key.themes = { DEFAULT_ICON_THEME, FALLBACK_ICON_THEME, "" };
    return ab->icon(key, { size, size });
}

std::shared_ptr<RImage> Gazette::loadAppIcon(const Notification &n) noexcept
{
    constexpr Int32 size { 64 };
    std::string icon { n.appIcon };

    if (icon.starts_with("file://"))
        icon = icon.substr(7);

    // A file
    if (!icon.empty() && icon.front() == '/')
    {
        auto ream { RCore::Get() };

        if (ream && ream->mainDevice())
        {
            const auto &formats { ream->mainDevice()->textureFormats().formats() };

            if (auto fmt { formats.find(DRM_FORMAT_ARGB8888) }; fmt != formats.end())
                if (auto image { RImage::LoadFile(icon, *fmt, { size * 2, size * 2 }) })
                    return image;
        }

        icon.clear();
    }

    if (!ab)
        return {};

    ABIconKey key;
    key.size = size;
    key.themes = { DEFAULT_ICON_THEME, FALLBACK_ICON_THEME, "" };
    key.appId = n.desktopEntry;

    if (!icon.empty())
        key.names.push_back(icon);

    if (!n.desktopEntry.empty())
    {
        if (xdgkit)
            if (auto entry { xdgkit->desktopEntry(n.desktopEntry) }; entry && !entry->icon.empty())
                key.names.push_back(entry->icon);

        key.names.push_back(n.desktopEntry);
    }

    if (!n.appName.empty())
    {
        std::string lower { n.appName };
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return std::tolower(c); });
        std::replace(lower.begin(), lower.end(), ' ', '-');
        key.names.push_back(lower);
    }

    key.names.push_back("dialog-information");
    key.names.push_back("application-x-executable");
    return ab->icon(key, { size, size });
}

/* ----------------------------------------- Settings ----------------------------------------- */

fs::path Gazette::SettingsPath() noexcept
{
    fs::path dir;

    if (const char *env { getenv("XDG_CONFIG_HOME") }; env && env[0] == '/')
        dir = env;
    else if (const char *home { getenv("HOME") })
        dir = fs::path(home) / ".config";

    return dir / "cz-gazette" / "gazette.conf";
}

void Gazette::loadSettings() noexcept
{
    std::error_code ec;

    if (!fs::is_regular_file(SettingsPath(), ec))
        return;

    auto ini { XDGINI::LoadFile(SettingsPath()) };

    if (!ini)
        return;

    if (auto group { ini->find("Gazette") }; group != ini->end())
        if (auto it { group->second.find("DoNotDisturb") }; it != group->second.end())
            m_dnd = it->second == "true";
}

void Gazette::saveSettings() noexcept
{
    const auto path { SettingsPath() };
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    const auto tmp { path.string() + ".tmp" };
    {
        std::ofstream out { tmp, std::ios::trunc };

        if (!out)
            return;

        out << "[Gazette]\nDoNotDisturb=" << (m_dnd ? "true" : "false") << '\n';
    }

    fs::rename(tmp, path, ec);
}
