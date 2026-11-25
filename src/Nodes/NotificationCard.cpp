#include <Nodes/NotificationCard.h>
#include <Core/Gazette.h>
#include <CZ/AK/AKTheme.h>
#include <CZ/Core/CZTimer.h>
#include <CZ/Core/Events/CZPointerButtonEvent.h>
#include <CZ/Core/Events/CZPointerEnterEvent.h>
#include <CZ/Core/Events/CZPointerLeaveEvent.h>
#include <linux/input-event-codes.h>
#include <sstream>

static constexpr SkScalar Padding { 12.f };
static constexpr SkScalar IconSize { 36.f };
static constexpr SkScalar ImageSize { 40.f };
static constexpr SkScalar Gap { 10.f };
static constexpr SkScalar FontSize { 13.f };
static constexpr SkScalar SmallFontSize { 11.5f };
static constexpr size_t MaxBodyLines { 4 };

static const CZAdaptiveColor ListColor { 0x80FFFFFF, 0x2EFFFFFF };
static const CZAdaptiveColor BannerShadowColor { 0x33000000, 0x66000000 };
static const CZAdaptiveColor ListShadowColor { 0x1F000000, 0x47000000 };

// Runs @p f once the current event is handled (it may destroy the card that got it)
static void Later(std::function<void()> f) noexcept
{
    CZTimer::OneShot(0, [f = std::move(f)](CZTimer*){ f(); });
}

static void Passive(AKNode &node) noexcept
{
    const SkRegion empty;
    node.setInputRegion(&empty);
}

static void SetupText(AKText &text, SkScalar size, SkFontStyle::Weight weight, CZAdaptiveColor color) noexcept
{
    Passive(text);
    text.setFontSize(size);
    text.setFontStyle(SkFontStyle(weight, SkFontStyle::kNormal_Width, SkFontStyle::kUpright_Slant));
    text.setTextColor(color);
}

// Width of @p text drawn like @p style (an AKText off-screen)
static SkScalar TextWidth(const AKText &style, const std::string &text) noexcept
{
    static AKText measure { "" };
    measure.setFontSize(style.fontSize());
    measure.setFontStyle(style.fontStyle());

    if (text.empty())
        return 0.f;

    measure.setText(text);
    return measure.layout().width().value;
}

// Byte offsets of the UTF-8 code points of @p s (and its size)
static std::vector<size_t> CodePoints(const std::string &s) noexcept
{
    std::vector<size_t> offsets;

    for (size_t i = 0; i < s.size(); i++)
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80)
            offsets.push_back(i);

    offsets.push_back(s.size());
    return offsets;
}

// @p text cut to fit @p width, ending with an ellipsis if cut
static std::string Elide(const AKText &style, const std::string &text, SkScalar width) noexcept
{
    if (TextWidth(style, text) <= width)
        return text;

    const auto cps { CodePoints(text) };
    size_t lo { 0 }, hi { cps.size() - 1 };

    // The longest prefix (in code points) that fits with the ellipsis
    while (lo < hi)
    {
        const size_t mid { (lo + hi + 1) / 2 };

        if (TextWidth(style, text.substr(0, cps[mid]) + "…") <= width)
            lo = mid;
        else
            hi = mid - 1;
    }

    std::string out { text.substr(0, cps[lo]) };

    while (!out.empty() && out.back() == ' ')
        out.pop_back();

    return out + "…";
}

// @p text wrapped to @p width (words, then code points if a word doesn't fit), at most @p maxLines
static std::string Wrap(const AKText &style, const std::string &text, SkScalar width, size_t maxLines) noexcept
{
    std::vector<std::string> lines;
    std::istringstream paragraphs { text };
    std::string paragraph;
    bool cut { false };

    while (!cut && std::getline(paragraphs, paragraph))
    {
        std::istringstream words { paragraph };
        std::string word, line;

        const auto push = [&](std::string l){
            if (lines.size() == maxLines)
                cut = true;
            else
                lines.push_back(std::move(l));
        };

        while (!cut && words >> word)
        {
            const std::string candidate { line.empty() ? word : line + " " + word };

            if (TextWidth(style, candidate) <= width)
            {
                line = candidate;
                continue;
            }

            if (!line.empty())
                push(std::move(line));

            line.clear();

            // A word wider than a line: split it
            while (!cut && TextWidth(style, word) > width)
            {
                const auto cps { CodePoints(word) };
                size_t n { 1 };

                while (n + 1 < cps.size() && TextWidth(style, word.substr(0, cps[n + 1])) <= width)
                    n++;

                push(word.substr(0, cps[n]));
                word = word.substr(cps[n]);
            }

            line = word;
        }

        if (!cut)
            push(std::move(line));
    }

    // Blank lines at the end
    while (!lines.empty() && lines.back().empty())
        lines.pop_back();

    if (cut && !lines.empty())
        lines.back() = Elide(style, lines.back() + " …", width);

    std::string out;

    for (size_t i = 0; i < lines.size(); i++)
    {
        if (i > 0)
            out.push_back('\n');

        out.append(lines[i]);
    }

    return out;
}

NotificationCard::NotificationCard(std::shared_ptr<Notification> notification, Style style, AKNode *parent) noexcept :
    AKRoundSolidColor(0, CZBorderRadius::Make(Radius), parent),
    m_style(style),
    m_shadow(style == Style::Banner ? 14.f : 6.f, SkIPoint::Make(0, style == Style::Banner ? 4 : 1), 0, false, this)
{
    m_shadow.setBorderRadius(CZBorderRadius::Make(Radius));
    setCursor(CZCursorShape::Default);
    layout().setWidth(Width);
    layout().setFlexShrink(0.f);
    layout().setFlexDirection(YGFlexDirectionColumn);
    layout().setPadding(YGEdgeAll, Padding);

    for (AKNode *n : std::initializer_list<AKNode*>{ &m_row, &m_texts, &m_header, &m_headerSpacer, &m_actions, &m_icon, &m_image })
        Passive(*n);

    m_row.layout().setGap(YGGutterColumn, Gap);
    m_row.layout().setAlignItems(YGAlignFlexStart);

    for (AKImageFrame *img : { &m_icon, &m_image })
    {
        img->setSizeMode(AKImageFrame::SizeMode::Contain);
        img->setAlignment(CZAlignCenter);
        img->layout().setFlexShrink(0.f);
    }

    m_icon.layout().setWidth(IconSize);
    m_icon.layout().setHeight(IconSize);
    m_image.layout().setWidth(ImageSize);
    m_image.layout().setHeight(ImageSize);

    m_texts.layout().setFlex(1.f);
    m_texts.layout().setGap(YGGutterRow, 1.f);
    m_header.layout().setAlignItems(YGAlignCenter);
    m_header.layout().setMinHeight(20.f);
    m_headerSpacer.layout().setFlex(1.f);

    SetupText(m_app, SmallFontSize, SkFontStyle::kMedium_Weight, AKTheme::SecondaryLabel);
    SetupText(m_time, SmallFontSize, SkFontStyle::kNormal_Weight, AKTheme::SecondaryLabel);
    SetupText(m_summary, FontSize, SkFontStyle::kSemiBold_Weight, AKTheme::Label);
    SetupText(m_body, FontSize, SkFontStyle::kNormal_Weight, AKTheme::Label);

    m_actions.layout().setGap(YGGutterColumn, 6.f);
    m_actions.layout().setMargin(YGEdgeTop, 10.f);

    // Close, while hovered, over the top left corner (like macOS). Only in the notification center:
    // banners are dragged away
    m_close.layout().setPositionType(YGPositionTypeAbsolute);
    m_close.setVisible(false);

    if (style == Style::Banner)
    {
        m_close.layout().setPosition(YGEdgeTop, 10.f);
        m_close.layout().setPosition(YGEdgeRight, 10.f);
    }
    else
    {
        enableChildrenClipping(false);
        m_close.layout().setPosition(YGEdgeTop, -CloseOverhang);
        m_close.layout().setPosition(YGEdgeLeft, -CloseOverhang);
    }

    m_close.onClicked = [this]{
        if (!m_notification)
            return;

        const UInt32 id { m_notification->id };
        Later([id]{ GetGazette()->close(id, Notification::CloseReason::Dismissed); });
    };

    setNotification(std::move(notification));
    updateColors();
}

void NotificationCard::setNotification(std::shared_ptr<Notification> notification) noexcept
{
    m_notification = std::move(notification);

    if (!m_notification)
        return;

    const Notification &n { *m_notification };
    m_icon.setImage(n.icon);
    m_image.setImage(n.picture);
    m_image.setVisible(n.picture != nullptr);

    const SkScalar textWidth { Width - 2 * Padding - IconSize - Gap - (n.picture ? ImageSize + Gap : 0.f) };
    m_app.setText(Elide(m_app, n.appName.empty() ? "Notification" : n.appName, textWidth - 70.f));
    updateTime();

    m_summary.setText(Elide(m_summary, n.summary, textWidth));
    m_summary.setVisible(!n.summary.empty());
    m_body.setText(Wrap(m_body, n.body, textWidth, MaxBodyLines));
    m_body.setVisible(!m_body.text().empty());

    m_buttons.clear();

    // Buttons (not "default": that's clicking the notification)
    for (const auto &[key, label] : n.actions)
    {
        if (key == "default")
            continue;

        auto &button { m_buttons.emplace_back(std::make_unique<AKButton>(label, &m_actions)) };
        button->layout().setFlex(1.f);
        button->setBackgroundColor(AKTheme::ControlFill);
        button->setHoverBackgroundColor(AKTheme::ControlHoverFill);
        button->setPressedBackgroundColor(AKTheme::ControlPressedFill);
        button->onClick.subscribe(this, [this, key](const CZPointerButtonEvent &e){
            if (e.pressed || !m_notification)
                return;

            const UInt32 id { m_notification->id };
            Later([id, key]{ GetGazette()->invoke(id, key); });
        });

        if (m_buttons.size() == 3)
            break;
    }

    m_actions.setVisible(!m_buttons.empty());
}

void NotificationCard::updateTime() noexcept
{
    if (m_notification)
        m_time.setText(m_notification->timeText());
}

void NotificationCard::setHovered(bool hovered) noexcept
{
    if (m_hovered == hovered)
        return;

    m_hovered = hovered;
    m_close.setVisible(hovered && m_style == Style::List); // Banners: dragged away instead

    if (!hovered)
        m_pressed = false;

    updateColors();
}

void NotificationCard::cancelPress() noexcept
{
    if (!m_pressed)
        return;

    m_pressed = false;
    updateColors();
}

void NotificationCard::pointerButtonEvent(const CZPointerButtonEvent &e)
{
    if (e.button != BTN_LEFT || !m_notification)
        return;

    if (e.pressed)
    {
        m_pressed = true;
        updateColors();
        return;
    }

    if (!m_pressed)
        return;

    m_pressed = false;
    updateColors();

    // The default action (e.g. the app opens the chat), else it's just dismissed
    const UInt32 id { m_notification->id };

    if (m_notification->hasDefault())
        Later([id]{ GetGazette()->invoke(id, "default"); });
    else
        Later([id]{ GetGazette()->close(id, Notification::CloseReason::Dismissed); });
}

void NotificationCard::colorSchemeEvent(const CZColorSchemeEvent &e)
{
    AKRoundSolidColor::colorSchemeEvent(e);
    updateColors();
}

void NotificationCard::updateColors() noexcept
{
    const CZColorScheme scheme { colorScheme() };
    // Not tinted while pressed
    const CZAdaptiveColor color { m_style == Style::Banner ? CZAdaptiveColor(0) : ListColor };
    setBackgroundColor(color.resolve(scheme));
    m_shadow.setColor((m_style == Style::Banner ? BannerShadowColor : ListShadowColor).resolve(scheme));
}

/* --------------------------------------- CloseButton --------------------------------------- */

// Solid (over the card's edge), with steps at least as visible as AKTheme's control fills
static const CZAdaptiveColor CloseColor { 0xFFF5F5F7, 0xFF48484C };
static const CZAdaptiveColor CloseHoverColor { 0xFFE2E2E6, 0xFF5C5C61 };
static const CZAdaptiveColor ClosePressedColor { 0xFFCDCDD2, 0xFF6E6E74 };

NotificationCard::CloseButton::CloseButton(AKNode *parent) noexcept :
    AKRoundSolidColor(0, CZBorderRadius::Make(Int32(Size / 2)), parent),
    m_shadow(4.f, SkIPoint::Make(0, 1), 0, false, this)
{
    m_shadow.setBorderRadius(CZBorderRadius::Make(Int32(Size / 2)));
    layout().setWidth(Size);
    layout().setHeight(Size);
    layout().setJustifyContent(YGJustifyCenter);
    layout().setAlignItems(YGAlignCenter);
    Passive(m_icon);
    updateColors();
}

void NotificationCard::CloseButton::pointerEnterEvent(const CZPointerEnterEvent &e)
{
    AKRoundSolidColor::pointerEnterEvent(e);
    m_hover = true;
    updateColors();
}

void NotificationCard::CloseButton::pointerLeaveEvent(const CZPointerLeaveEvent &e)
{
    AKRoundSolidColor::pointerLeaveEvent(e);
    m_hover = m_pressed = false;
    updateColors();
}

void NotificationCard::CloseButton::pointerButtonEvent(const CZPointerButtonEvent &e)
{
    if (e.button != BTN_LEFT)
        return;

    if (e.pressed)
        m_pressed = true;
    else if (m_pressed)
    {
        m_pressed = false;

        if (onClicked)
            onClicked();
    }

    updateColors();
}

void NotificationCard::CloseButton::colorSchemeEvent(const CZColorSchemeEvent &e)
{
    AKRoundSolidColor::colorSchemeEvent(e);
    updateColors();
}

void NotificationCard::CloseButton::updateColors() noexcept
{
    const CZColorScheme scheme { colorScheme() };
    setBackgroundColor((m_pressed ? ClosePressedColor : m_hover ? CloseHoverColor : CloseColor).resolve(scheme));
    m_shadow.setColor(ListShadowColor.resolve(scheme));
    m_icon.setColor((m_hover ? AKTheme::Label : AKTheme::SecondaryLabel).resolve(scheme));
}
