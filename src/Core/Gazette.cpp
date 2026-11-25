#include <Core/Gazette.h>
#include <Core/Log.h>
#include <Surfaces/BannerSurface.h>
#include <Surfaces/CenterSurface.h>
#include <CZ/Marco/MApp.h>
#include <CZ/Marco/MScreen.h>
#include <CZ/Core/CZTimer.h>
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
    center = std::make_unique<CenterSurface>();

    // Banners on every screen
    for (MScreen *screen : marco->screens())
        screenPlugged(*screen);

    marco->onScreenPlugged.subscribe(this, [this](MScreen &screen){ screenPlugged(screen); });
    marco->onScreenUnplugged.subscribe(this, [this](MScreen &screen){ screenUnplugged(screen); });

    // The D-Bus side: Gazette shows what it gets
    SVNotificationsServer::Handlers handlers;
    handlers.notify = [this](const SVNotification &n){ return notify(n); };
    handlers.close = [this](UInt32 id){ close(id, Notification::CloseReason::Closed); };
    handlers.centerState = [this]{
        const bool shown { centerShown() };
        MScreen *screen { shown ? center->screen() : nullptr };
        return SVNotificationsServer::CenterState { shown, m_dnd, UInt32(m_notifications.size()), screen ? screen->props().name : std::string() };
    };
    handlers.toggleCenter = [this](const std::string &screen){ toggleCenter(screen); };
    handlers.showCenter = [this](const std::string &screen){ showCenter(screen); };
    handlers.hideCenter = [this]{ hideCenter(); };
    handlers.setDoNotDisturb = [this](bool enabled){ setDoNotDisturb(enabled); };

    server = SVNotificationsServer::GetOrMake();
    server->start(handlers, { .name = "Gazette", .vendor = "Cuarzo", .version = "0.1.0",
                              .capabilities = { "actions", "body", "body-markup", "icon-static", "persistence" } });
    onStateChanged.subscribe(this, [this]{ server->emitCenterState(); });
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

void Gazette::screenPlugged(MScreen &screen) noexcept
{
    for (const auto &b : banners)
        if (b->bannerScreen() == &screen)
            return;

    banners.emplace_back(std::make_unique<BannerSurface>(&screen));
}

void Gazette::screenUnplugged(MScreen &screen) noexcept
{
    const auto it { std::find_if(banners.begin(), banners.end(), [&screen](const auto &b){ return b->bannerScreen() == &screen; }) };

    if (it == banners.end())
        return;

    // Destroyed later: callbacks of its banners may still be queued (they find none by then)
    std::shared_ptr<BannerSurface> retired { std::move(*it) };
    banners.erase(it);
    retired->retire();
    CZTimer::OneShot(1000, [retired](CZTimer*){});
}

Gazette::~Gazette() noexcept
{
    sd_bus_slot_unref(m_exposeMatch);
    sd_bus_slot_unref(m_workspaceMatch);
    server.reset();
    center.reset();
    banners.clear();
    _gazette = nullptr;
}

/* -------------------------------------- Notifications -------------------------------------- */

// image-data: premultiplied, as an image
static std::shared_ptr<RImage> MakeImage(const SVNotification::Image &image) noexcept
{
    auto ream { RCore::Get() };

    if (!image.valid() || !ream || !ream->mainDevice())
        return {};

    const auto &formats { ream->mainDevice()->textureFormats().formats() };
    const auto format { formats.find(DRM_FORMAT_ABGR8888) }; // R, G, B, A bytes

    if (format == formats.end())
        return {};

    std::vector<UInt8> rgba(image.rgba.size());

    for (size_t i = 0; i < rgba.size(); i += 4)
    {
        const UInt32 a { image.rgba[i + 3] };
        rgba[i + 0] = UInt8((image.rgba[i + 0] * a + 127) / 255);
        rgba[i + 1] = UInt8((image.rgba[i + 1] * a + 127) / 255);
        rgba[i + 2] = UInt8((image.rgba[i + 2] * a + 127) / 255);
        rgba[i + 3] = UInt8(a);
    }

    RPixelBufferInfo info {};
    info.pixels = rgba.data();
    info.stride = UInt32(image.width * 4);
    info.format = format->format();
    info.size = SkISize::Make(image.width, image.height);
    info.alphaType = kPremul_SkAlphaType;
    return RImage::MakeFromPixels(info, *format);
}

// image-path: a file (or file:// URI), else an icon name
static std::shared_ptr<RImage> LoadImagePath(std::string path) noexcept
{
    if (path.starts_with("file://"))
        path = path.substr(7);

    if (path.empty() || path.front() != '/')
        return GetGazette()->loadIcon(path, 64);

    auto ream { RCore::Get() };

    if (!ream || !ream->mainDevice())
        return {};

    const auto &formats { ream->mainDevice()->textureFormats().formats() };

    if (auto fmt { formats.find(DRM_FORMAT_ARGB8888) }; fmt != formats.end())
        return RImage::LoadFile(path, *fmt, { 256, 256 });

    return {};
}

UInt32 Gazette::notify(const SVNotification &sent) noexcept
{
    auto n { std::make_shared<Notification>() };
    static_cast<SVNotification&>(*n) = sent;

    // Shown as plain text (body-markup is removed)
    n->summary = SVNotificationsServer::PlainText(n->summary);
    n->body = SVNotificationsServer::PlainText(n->body);

    // An icon name as image-path (e.g. notify-send -i sends it so, app_icon empty): the app's icon
    std::string imagePath { n->imagePath };
    const bool imageIsFile { imagePath.starts_with("/") || imagePath.starts_with("file://") };

    if (!imagePath.empty() && !imageIsFile && (n->appIcon.empty() || n->appIcon == imagePath))
    {
        n->appIcon = imagePath;
        imagePath.clear();
    }
    else if (imagePath == n->appIcon)
        imagePath.clear();

    n->picture = n->image.valid() ? MakeImage(n->image) : (imagePath.empty() ? nullptr : LoadImagePath(imagePath));
    n->image = {}; // Not kept (the picture is)

    // The app's name from its .desktop entry if not given
    if (n->appName.empty() && !n->desktopEntry.empty() && xdgkit)
        if (auto entry { xdgkit->desktopEntry(n->desktopEntry) })
            n->appName = entry->name;

    return add(n, n->replaces);
}

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
            for (auto &b : banners)
                b->replace(n);
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
        for (auto &b : banners)
            b->show(n);

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
    for (auto &b : banners)
        b->remove(id);
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

void Gazette::bannerDismissed(UInt32 id) noexcept
{
    for (auto &b : banners)
        b->remove(id);

    bannerExpired(id);
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
        for (auto &b : banners)
            b->hideAll();

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
    for (auto &b : banners)
        b->hideAll();
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
