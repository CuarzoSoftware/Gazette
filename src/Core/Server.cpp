#include <Core/Server.h>
#include <Core/Gazette.h>
#include <Core/Log.h>
#include <CZ/Core/CZBus.h>
#include <CZ/Ream/RCore.h>
#include <CZ/Ream/RDevice.h>
#include <CZ/Ream/RImage.h>
#include <CZ/XDG/XDGKit.h>
#include <CZ/XDG/XDGDesktopEntry.h>
#include <systemd/sd-bus.h>
#include <drm_fourcc.h>
#include <cstring>
#include <vector>

static constexpr const char *NotificationsPath { "/org/freedesktop/Notifications" };
static constexpr const char *NotificationsInterface { "org.freedesktop.Notifications" };
static constexpr const char *GazettePath { "/org/cuarzo/Gazette" };
static constexpr const char *GazetteInterface { "org.cuarzo.Gazette" };

/* ----------------------------------------- Helpers ----------------------------------------- */

std::string Server::PlainText(const std::string &markup) noexcept
{
    std::string out;
    out.reserve(markup.size());

    for (size_t i = 0; i < markup.size(); i++)
    {
        const char c { markup[i] };

        // A tag: <b>, </i>, <a href="...">, <img .../>, <br/> (a line break)
        if (c == '<')
        {
            const size_t end { markup.find('>', i) };

            if (end == std::string::npos)
            {
                out.push_back(c);
                continue;
            }

            const std::string tag { markup.substr(i + 1, end - i - 1) };

            if (tag.starts_with("br"))
                out.push_back('\n');

            i = end;
            continue;
        }

        if (c == '&')
        {
            static const std::pair<const char*, const char*> entities[] {
                { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&apos;", "'" }, { "&#39;", "'" }, { "&nbsp;", " " } };
            bool decoded { false };

            for (const auto &[entity, value] : entities)
            {
                if (markup.compare(i, std::strlen(entity), entity) == 0)
                {
                    out.append(value);
                    i += std::strlen(entity) - 1;
                    decoded = true;
                    break;
                }
            }

            if (decoded)
                continue;
        }

        out.push_back(c);
    }

    // Trailing blank lines / spaces
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ' || out.back() == '\r'))
        out.pop_back();

    return out;
}

// image-data: (iiibiiay) width, height, rowstride, has_alpha, bits_per_sample, channels, data (RGB(A))
static std::shared_ptr<RImage> ReadImageData(sd_bus_message *m) noexcept
{
    Int32 width, height, stride, hasAlpha, bits, channels;
    const void *data {};
    size_t size {};

    if (sd_bus_message_enter_container(m, 'r', "iiibiiay") <= 0)
        return {};

    const bool ok { sd_bus_message_read(m, "iiibii", &width, &height, &stride, &hasAlpha, &bits, &channels) >= 0 &&
                    sd_bus_message_read_array(m, 'y', &data, &size) >= 0 };
    sd_bus_message_exit_container(m);

    if (!ok || width <= 0 || height <= 0 || width > 4096 || height > 4096 || bits != 8 ||
        (channels != 3 && channels != 4) || stride < width * channels || size < size_t(stride) * size_t(height - 1) + size_t(width * channels))
        return {};

    auto ream { RCore::Get() };

    if (!ream || !ream->mainDevice())
        return {};

    const auto &formats { ream->mainDevice()->textureFormats().formats() };
    const auto format { formats.find(DRM_FORMAT_ABGR8888) }; // R, G, B, A bytes

    if (format == formats.end())
        return {};

    std::vector<UInt8> rgba(size_t(width) * size_t(height) * 4);
    const auto *src { static_cast<const UInt8*>(data) };

    for (Int32 y = 0; y < height; y++)
    {
        const UInt8 *row { src + size_t(y) * size_t(stride) };
        UInt8 *dst { &rgba[size_t(y) * size_t(width) * 4] };

        for (Int32 x = 0; x < width; x++)
        {
            const UInt8 *p { row + x * channels };
            const UInt8 a { channels == 4 && hasAlpha ? p[3] : UInt8(255) };

            // Premultiplied
            dst[x * 4 + 0] = UInt8((UInt32(p[0]) * a + 127) / 255);
            dst[x * 4 + 1] = UInt8((UInt32(p[1]) * a + 127) / 255);
            dst[x * 4 + 2] = UInt8((UInt32(p[2]) * a + 127) / 255);
            dst[x * 4 + 3] = a;
        }
    }

    RPixelBufferInfo info {};
    info.pixels = rgba.data();
    info.stride = UInt32(width * 4);
    info.format = format->format();
    info.size = SkISize::Make(width, height);
    info.alphaType = kPremul_SkAlphaType;
    return RImage::MakeFromPixels(info, *format);
}

static std::shared_ptr<RImage> LoadImagePath(std::string path) noexcept
{
    if (path.starts_with("file://"))
        path = path.substr(7);

    if (path.empty() || path.front() != '/')
        return GetGazette()->loadIcon(path, 64); // An icon name

    auto ream { RCore::Get() };

    if (!ream || !ream->mainDevice())
        return {};

    const auto &formats { ream->mainDevice()->textureFormats().formats() };

    if (auto fmt { formats.find(DRM_FORMAT_ARGB8888) }; fmt != formats.end())
        return RImage::LoadFile(path, *fmt, { 256, 256 });

    return {};
}

/* -------------------------------- org.freedesktop.Notifications -------------------------------- */

static int Notify(sd_bus_message *m, void *, sd_bus_error *)
{
    const char *appName {}, *appIcon {}, *summary {}, *body {};
    UInt32 replacesId { 0 };

    int r { sd_bus_message_read(m, "susss", &appName, &replacesId, &appIcon, &summary, &body) };

    if (r < 0)
        return r;

    auto n { std::make_shared<Notification>() };
    n->appName = appName ? appName : "";
    n->appIcon = appIcon ? appIcon : "";
    n->summary = Server::PlainText(summary ? summary : "");
    n->body = Server::PlainText(body ? body : "");

    // Actions: key, label, key, label...
    if ((r = sd_bus_message_enter_container(m, 'a', "s")) < 0)
        return r;

    std::vector<std::string> actions;
    const char *str {};

    while (sd_bus_message_read(m, "s", &str) > 0)
        actions.emplace_back(str ? str : "");

    sd_bus_message_exit_container(m);

    for (size_t i = 0; i + 1 < actions.size(); i += 2)
    {
        if (actions[i] == "default")
            n->hasDefault = true;
        else
            n->actions.push_back({ actions[i], actions[i + 1] });
    }

    // Hints
    std::shared_ptr<RImage> imageData;
    std::string imagePath;

    if ((r = sd_bus_message_enter_container(m, 'a', "{sv}")) < 0)
        return r;

    while (sd_bus_message_enter_container(m, 'e', "sv") > 0)
    {
        const char *key {};
        sd_bus_message_read(m, "s", &key);
        const std::string k { key ? key : "" };
        const char *contents {};
        char type {};
        sd_bus_message_peek_type(m, &type, &contents);
        const std::string sig { contents ? contents : "" };
        bool read { false };

        if (sd_bus_message_enter_container(m, 'v', contents) > 0)
        {
            if (k == "urgency" && sig == "y")
            {
                UInt8 u { 1 };
                sd_bus_message_read(m, "y", &u);
                n->urgency = Notification::Urgency(std::min<UInt8>(u, 2));
                read = true;
            }
            else if ((k == "desktop-entry" || k == "image-path" || k == "image_path") && sig == "s")
            {
                sd_bus_message_read(m, "s", &str);

                if (k == "desktop-entry")
                    n->desktopEntry = str ? str : "";
                else
                    imagePath = str ? str : "";

                read = true;
            }
            else if ((k == "transient" || k == "resident") && sig == "b")
            {
                int b { 0 };
                sd_bus_message_read(m, "b", &b);
                (k == "transient" ? n->transient : n->resident) = b;
                read = true;
            }
            else if ((k == "image-data" || k == "image_data" || k == "icon_data") && sig == "(iiibiiay)")
            {
                // image-data wins over the older names
                if (auto image { ReadImageData(m) }; image && (!imageData || k == "image-data"))
                    imageData = image;

                read = true;
            }

            if (!read)
                sd_bus_message_skip(m, contents);

            sd_bus_message_exit_container(m);
        }

        sd_bus_message_exit_container(m);
    }

    sd_bus_message_exit_container(m);

    Int32 timeout { -1 };
    sd_bus_message_read(m, "i", &timeout);
    n->timeout = timeout;

    // An icon name as image-path (e.g. notify-send -i sends it so, app_icon empty): the app's icon
    const bool imageIsFile { imagePath.starts_with("/") || imagePath.starts_with("file://") };

    if (!imagePath.empty() && !imageIsFile && (n->appIcon.empty() || n->appIcon == imagePath))
    {
        n->appIcon = imagePath;
        imagePath.clear();
    }
    else if (imagePath == n->appIcon)
        imagePath.clear();

    n->image = imageData ? imageData : (imagePath.empty() ? nullptr : LoadImagePath(imagePath));

    // The app's name from its .desktop entry if not given
    if (n->appName.empty() && !n->desktopEntry.empty())
        if (auto *g { GetGazette() }; g->xdgkit)
            if (auto entry { g->xdgkit->desktopEntry(n->desktopEntry) })
                n->appName = entry->name;

    const UInt32 id { GetGazette()->add(n, replacesId) };
    return sd_bus_reply_method_return(m, "u", id);
}

static int CloseNotification(sd_bus_message *m, void *, sd_bus_error *)
{
    UInt32 id { 0 };

    if (int r = sd_bus_message_read(m, "u", &id); r < 0)
        return r;

    GetGazette()->close(id, Notification::CloseReason::Closed);
    return sd_bus_reply_method_return(m, "");
}

static int GetCapabilities(sd_bus_message *m, void *, sd_bus_error *)
{
    sd_bus_message *reply {};

    if (int r = sd_bus_message_new_method_return(m, &reply); r < 0)
        return r;

    static const char *caps[] { "actions", "body", "body-markup", "icon-static", "persistence", nullptr };
    sd_bus_message_append_strv(reply, const_cast<char**>(caps));
    const int r { sd_bus_send(nullptr, reply, nullptr) };
    sd_bus_message_unref(reply);
    return r;
}

static int GetServerInformation(sd_bus_message *m, void *, sd_bus_error *)
{
    return sd_bus_reply_method_return(m, "ssss", "Gazette", "Cuarzo", "0.1.0", "1.2");
}

static const sd_bus_vtable NotificationsVTable[]
{
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", Notify, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("CloseNotification", "u", "", CloseNotification, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetCapabilities", "", "as", GetCapabilities, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetServerInformation", "", "ssss", GetServerInformation, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
    SD_BUS_SIGNAL("ActionInvoked", "us", 0),
    SD_BUS_VTABLE_END
};

/* ------------------------------------- org.cuarzo.Gazette ------------------------------------- */

static int ToggleCenter(sd_bus_message *m, void *, sd_bus_error *)
{
    const char *screen {};
    sd_bus_message_read(m, "s", &screen);
    GetGazette()->toggleCenter(screen ? screen : "");
    return sd_bus_reply_method_return(m, "");
}

static int ShowCenter(sd_bus_message *m, void *, sd_bus_error *)
{
    const char *screen {};
    sd_bus_message_read(m, "s", &screen);
    GetGazette()->showCenter(screen ? screen : "");
    return sd_bus_reply_method_return(m, "");
}

static int HideCenter(sd_bus_message *m, void *, sd_bus_error *)
{
    GetGazette()->hideCenter();
    return sd_bus_reply_method_return(m, "");
}

static int SetDoNotDisturb(sd_bus_message *m, void *, sd_bus_error *)
{
    int enabled { 0 };

    if (int r = sd_bus_message_read(m, "b", &enabled); r < 0)
        return r;

    GetGazette()->setDoNotDisturb(enabled);
    return sd_bus_reply_method_return(m, "");
}

static int GetState(sd_bus_message *m, void *, sd_bus_error *)
{
    auto *g { GetGazette() };
    return sd_bus_reply_method_return(m, "bbu", int(g->centerShown()), int(g->doNotDisturb()), UInt32(g->notifications().size()));
}

static const sd_bus_vtable GazetteVTable[]
{
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("ToggleCenter", "s", "", ToggleCenter, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("ShowCenter", "s", "", ShowCenter, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("HideCenter", "", "", HideCenter, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("SetDoNotDisturb", "b", "", SetDoNotDisturb, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetState", "", "bbu", GetState, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("StateChanged", "bbu", 0),
    SD_BUS_VTABLE_END
};

/* ------------------------------------------ Server ------------------------------------------ */

Server::Server() noexcept
{
    m_bus = CZBus::GetOrMakeUser();

    if (!m_bus)
    {
        Log(CZError, CZLN, "No user bus: no notifications");
        return;
    }

    sd_bus *bus { m_bus->bus() };
    sd_bus_add_object_vtable(bus, &m_notificationsSlot, NotificationsPath, NotificationsInterface, NotificationsVTable, nullptr);
    sd_bus_add_object_vtable(bus, &m_gazetteSlot, GazettePath, GazetteInterface, GazetteVTable, nullptr);

    // Taken from another daemon if it allows it (most do)
    if (int r = sd_bus_request_name(bus, NotificationsInterface, SD_BUS_NAME_REPLACE_EXISTING); r < 0)
        Log(CZError, CZLN, "Couldn't own {} (another notifications daemon running?): {}", NotificationsInterface, strerror(-r));

    if (int r = sd_bus_request_name(bus, GazetteInterface, 0); r < 0)
        Log(CZError, CZLN, "Couldn't own {}: {}", GazetteInterface, strerror(-r));

    GetGazette()->onStateChanged.subscribe(this, [this]{ emitState(); });
}

Server::~Server() noexcept
{
    sd_bus_slot_unref(m_notificationsSlot);
    sd_bus_slot_unref(m_gazetteSlot);
}

void Server::emitClosed(UInt32 id, Notification::CloseReason reason) noexcept
{
    if (m_bus)
        sd_bus_emit_signal(m_bus->bus(), NotificationsPath, NotificationsInterface, "NotificationClosed", "uu", id, UInt32(reason));
}

void Server::emitActionInvoked(UInt32 id, const std::string &action) noexcept
{
    if (m_bus)
        sd_bus_emit_signal(m_bus->bus(), NotificationsPath, NotificationsInterface, "ActionInvoked", "us", id, action.c_str());
}

void Server::emitState() noexcept
{
    auto *g { GetGazette() };

    if (m_bus)
        sd_bus_emit_signal(m_bus->bus(), GazettePath, GazetteInterface, "StateChanged", "bbu",
            int(g->centerShown()), int(g->doNotDisturb()), UInt32(g->notifications().size()));
}
