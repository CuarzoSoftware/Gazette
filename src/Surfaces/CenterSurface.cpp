#include <Surfaces/CenterSurface.h>
#include <Core/Gazette.h>
#include <CZ/Marco/MScreen.h>
#include <CZ/AK/AKApp.h>
#include <CZ/AK/AKTheme.h>
#include <CZ/Core/CZEase.h>
#include <CZ/Core/Events/CZPointerButtonEvent.h>
#include <CZ/Core/Events/CZPointerMoveEvent.h>
#include <CZ/Core/Events/CZPointerLeaveEvent.h>
#include <CZ/Core/Events/CZKeyboardKeyEvent.h>
#include <CZ/skia/core/SkRRect.h>
#include <linux/input-event-codes.h>
#include <algorithm>

static constexpr SkScalar Margin { 6.f };      // From the top bar and the screen's edges
static constexpr SkScalar Padding { 12.f };
static constexpr SkScalar Gap { 10.f };
static constexpr SkScalar PanelWidth { NotificationCard::Width + 2 * Padding };
static constexpr Int32 PanelRadius { 18 };

// A subtle blur: what's behind stays recognizable (the cards keep their own fill)
static const MBlurMaterial PanelMaterial { 12.f, 1.5f, { 0x99EEEEEE, 0x99222222 } };
static constexpr UInt32 ClockMs { 30000 };
static constexpr SkScalar ListGap { 10.f + NotificationCard::CloseOverhang * 0.5f };
static constexpr UInt32 RemoveMs { 360 };    // Slides out (first half), then its room collapses
static constexpr SkScalar HeaderHeight { 28.f }; // Fixed: Clear All comes and goes without moving the rest

static const CZAdaptiveColor CardColor { 0x80FFFFFF, 0x2EFFFFFF };
static const CZAdaptiveColor BadgeOffColor { 0x1F000000, 0x2EFFFFFF };

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

CenterSurface::Card::Card(AKNode *parent) noexcept : AKRoundSolidColor(0, CZBorderRadius::Make(12), parent)
{
    Passive(*this);
    updateColors();
}

void CenterSurface::Card::colorSchemeEvent(const CZColorSchemeEvent &e)
{
    AKRoundSolidColor::colorSchemeEvent(e);
    updateColors();
}

void CenterSurface::Card::updateColors() noexcept
{
    setBackgroundColor(CardColor.resolve(colorScheme()));
}

CenterSurface::CenterSurface() noexcept
{
    setColor(CZAdaptiveColor(0));
    setBlurMaterial(PanelMaterial);
    setBorderRadius(CZBorderRadius::Make(0));
    setScope("cz-gazette-center");
    setLayer(Layer::Overlay);
    setKeyboardInteractivity(KeyboardInteractivity::OnDemand); // Escape, once clicked
    setAnchor(CZEdgeTop | CZEdgeRight | CZEdgeBottom | CZEdgeLeft);
    setExclusiveZone(0); // Below the top bar
    requestAvailableWidth();
    requestAvailableHeight();

    // The panel, at the right edge (moved right while sliding, see setSlide())
    m_panel.layout().setFlexDirection(YGFlexDirectionColumn);
    m_panel.layout().setPositionType(YGPositionTypeAbsolute);
    m_panel.layout().setPosition(YGEdgeTop, Margin);
    m_panel.layout().setPosition(YGEdgeBottom, Margin);
    m_panel.layout().setPosition(YGEdgeRight, Margin);
    m_panel.layout().setWidth(PanelWidth);
    m_panel.layout().setPadding(YGEdgeVertical, Padding);
    m_panel.layout().setGap(YGGutterRow, Gap);

    // Header: title and Clear All
    m_header.layout().setAlignItems(YGAlignCenter);
    m_header.layout().setHeight(HeaderHeight);
    m_header.layout().setPadding(YGEdgeHorizontal, 4.f);
    m_header.layout().setMargin(YGEdgeHorizontal, Padding);
    Passive(m_header);
    Passive(m_headerSpacer);
    m_headerSpacer.layout().setFlex(1.f);
    SetupText(m_title, 17.f, SkFontStyle::kBold_Weight, AKTheme::Label);
    m_clear.layout().setHeight(24.f);
    m_clear.setBackgroundColor(AKTheme::ControlFill);
    m_clear.setHoverBackgroundColor(AKTheme::ControlHoverFill);
    m_clear.setPressedBackgroundColor(AKTheme::ControlPressedFill);
    m_clear.onClick.subscribe(this, [](const CZPointerButtonEvent &e){
        if (!e.pressed)
            CZTimer::OneShot(0, [](CZTimer*){ GetGazette()->clearAll(); });
    });

    // Do Not Disturb
    m_dndCard.layout().setFlexDirection(YGFlexDirectionRow);
    m_dndCard.layout().setMargin(YGEdgeHorizontal, Padding);
    m_dndCard.layout().setAlignItems(YGAlignCenter);
    m_dndCard.layout().setPadding(YGEdgeAll, 10.f);
    m_dndCard.layout().setGap(YGGutterColumn, 10.f);
    Passive(m_dndBadge);
    m_dndBadge.layout().setWidth(30.f);
    m_dndBadge.layout().setHeight(30.f);
    m_dndBadge.layout().setJustifyContent(YGJustifyCenter);
    m_dndBadge.layout().setAlignItems(YGAlignCenter);
    Passive(m_dndIcon);
    Passive(m_dndTexts);
    m_dndTexts.layout().setFlex(1.f);
    SetupText(m_dndTitle, 13.f, SkFontStyle::kSemiBold_Weight, AKTheme::Label);
    SetupText(m_dndSubtitle, 11.5f, SkFontStyle::kNormal_Weight, AKTheme::SecondaryLabel);
    m_dndSwitch.onToggled.subscribe(this, [](bool on){ GetGazette()->setDoNotDisturb(on); });

    // The list (scrolls)
    m_scroll.layout().setFlex(1.f);
    m_scroll.layout().setMinHeight(0.f);
    m_list.layout().setPadding(YGEdgeBottom, 4.f);

    // Room for the cards' close buttons (over their top left corner)
    m_list.layout().setPadding(YGEdgeHorizontal, Padding);
    m_list.layout().setPadding(YGEdgeTop, NotificationCard::CloseOverhang);
    m_list.layout().setGap(YGGutterRow, ListGap);
    Passive(m_list);
    SetupText(m_empty, 13.f, SkFontStyle::kMedium_Weight, AKTheme::SecondaryLabel);
    m_empty.layout().setAlignSelf(YGAlignCenter);
    m_empty.layout().setMargin(YGEdgeTop, 24.f);

    m_anim.onUpdate.subscribe(this, [this](CZAnimation *a){
        setSlide(m_slideFrom + (m_slideTo - m_slideFrom) * SkScalar(CZEase::OutCubic(a->value())));
    });
    m_anim.onFinish.subscribe(this, [this](CZAnimation*){
        if (!m_shown)
            setMapped(false);
    });

    m_clock.setCallback([this](CZTimer *timer){
        for (auto &card : m_cards)
            card->updateTime();

        timer->start(ClockMs);
    });

    GetGazette()->onNotificationsChanged.subscribe(this, [this]{ rebuild(); });
    GetGazette()->onStateChanged.subscribe(this, [this]{ updateState(); });
    rebuild();
    updateState();
    setSlide(PanelWidth + Margin);
}

void CenterSurface::show(MScreen *screen) noexcept
{
    if (m_shown)
        return;

    m_shown = true;

    if (!mapped())
    {
        if (screen)
            setScreen(screen);

        setSlide(PanelWidth + Margin);
        setMapped(true);
    }

    for (auto &card : m_cards)
        card->updateTime();

    m_scroll.setOffsetY(0);
    m_clock.start(ClockMs);
    slideTo(0.f, ShowMs);
}

void CenterSurface::hide() noexcept
{
    if (!m_shown)
        return;

    m_shown = false;
    m_pressedOutside = false;
    m_clock.stop();

    for (auto &card : m_cards)
        card->setHovered(false);

    slideTo(PanelWidth + Margin, HideMs);
}

void CenterSurface::slideTo(SkScalar to, UInt32 ms) noexcept
{
    // From where it is (e.g. hidden while still sliding in)
    m_slideFrom = m_slide;
    m_slideTo = to;
    m_anim.pause();
    m_anim.setDuration(ms);
    m_anim.reset();
    m_anim.start();
}

void CenterSurface::setSlide(SkScalar slide) noexcept
{
    m_slide = slide;
    m_panel.layout().setPosition(YGEdgeRight, Margin - slide);
    update();
}

void CenterSurface::rebuild() noexcept
{
    // The cards stay (removed ones leave animated, new ones go at the top)
    std::vector<std::shared_ptr<Notification>> shown;

    for (const auto &n : GetGazette()->notifications())
        if (!n->transient)
            shown.push_back(n);

    std::vector<std::unique_ptr<NotificationCard>> cards;

    for (const auto &n : shown)
    {
        auto it { std::find_if(m_cards.begin(), m_cards.end(), [&n](const auto &c){ return c && c->notification()->id == n->id; }) };

        if (it != m_cards.end())
        {
            if ((*it)->notification() != n)
                (*it)->setNotification(n); // Replaced

            cards.emplace_back(std::move(*it));
        }
        else
        {
            auto card { std::make_unique<NotificationCard>(n, NotificationCard::Style::List, &m_list) };

            if (m_list.children().size() > 1)
                card->insertBefore(m_list.children().front());

            cards.emplace_back(std::move(card));
        }
    }

    for (auto &card : m_cards)
        if (card)
            removeCard(std::move(card));

    m_cards = std::move(cards);
    updateEmpty();
    update();
}

void CenterSurface::removeCard(std::unique_ptr<NotificationCard> card) noexcept
{
    // Hidden: gone right away
    if (!m_shown || !mapped())
        return;

    auto removing { std::make_unique<Removing>() };
    Removing *r { removing.get() };
    r->height = card->layout().calculatedHeight();
    r->card = std::move(card);
    r->card->setHovered(false);
    r->card->enableChildrenClipping(true);
    const SkRegion empty;
    r->card->setInputRegion(&empty);

    r->anim.setDuration(RemoveMs);
    r->anim.onUpdate.subscribe(this, [this, r](CZAnimation *a){
        const Float64 t { a->value() };
        NotificationCard &c { *r->card };

        if (t < 0.5)
            c.layout().setPosition(YGEdgeLeft, (PanelWidth + Margin) * SkScalar(CZEase::InOutQuad(t * 2.0)));
        else
        {
            // Its room (and the gap after it) closes
            const SkScalar u { SkScalar(CZEase::InOutQuad((t - 0.5) * 2.0)) };
            c.setVisible(false);
            c.layout().setHeight(r->height * (1.f - u));
            c.layout().setMargin(YGEdgeBottom, -ListGap * u);
        }

        update();
    });
    r->anim.onFinish.subscribe(this, [this, r](CZAnimation*){
        CZTimer::OneShot(0, [this, r](CZTimer*){
            std::erase_if(m_removing, [r](const auto &x){ return x.get() == r; });
            updateEmpty();
            update();
        });
    });

    m_removing.emplace_back(std::move(removing));
    r->anim.start();
}

void CenterSurface::updateEmpty() noexcept
{
    const bool empty { m_cards.empty() && m_removing.empty() };
    m_empty.setVisible(empty);
    m_scroll.setVisible(!empty);
    m_clear.setVisible(!m_cards.empty());
}

void CenterSurface::updateState() noexcept
{
    const bool dnd { GetGazette()->doNotDisturb() };

    if (m_dndSwitch.on() != dnd)
        m_dndSwitch.setOn(dnd);

    m_dndSubtitle.setText(dnd ? "On: no banners" : "Off");
    m_dndBadge.setBackgroundColor((dnd ? AKTheme::SystemIndigo : BadgeOffColor).resolve(colorScheme()));
    m_dndIcon.setColor((dnd ? AKTheme::White : AKTheme::Label).resolve(colorScheme()));
}

void CenterSurface::updateHover() noexcept
{
    const SkPoint pos { AKApp::Get()->pointer().pos() };

    for (auto &card : m_cards)
    {
        // Its close button included (over the top left corner)
        SkRect rect { SkRect::Make(card->worldRect()) };
        rect.fLeft -= NotificationCard::CloseOverhang;
        rect.fTop -= NotificationCard::CloseOverhang;
        card->setHovered(m_shown && rect.contains(pos.x(), pos.y()) &&
                         SkRect::Make(m_scroll.worldRect()).contains(pos.x(), pos.y()));
    }
}

void CenterSurface::pointerMoveEvent(const CZPointerMoveEvent &e)
{
    MLayerSurface::pointerMoveEvent(e);
    updateHover();
}

void CenterSurface::pointerLeaveEvent(const CZPointerLeaveEvent &e)
{
    MLayerSurface::pointerLeaveEvent(e);

    for (auto &card : m_cards)
        card->setHovered(false);
}

void CenterSurface::pointerButtonEvent(const CZPointerButtonEvent &e)
{
    MLayerSurface::pointerButtonEvent(e);

    if (e.button != BTN_LEFT)
        return;

    // Outside the panel: hides it (on release, the press may start elsewhere)
    const SkPoint pos { AKApp::Get()->pointer().pos() };
    const bool outside { !SkRect::Make(m_panel.worldRect()).contains(pos.x(), pos.y()) };

    if (e.pressed)
        m_pressedOutside = outside;
    else if (m_pressedOutside && outside)
        GetGazette()->hideCenter();
}

void CenterSurface::keyboardKeyEvent(const CZKeyboardKeyEvent &e)
{
    MLayerSurface::keyboardKeyEvent(e);

    if (e.isPressed && e.code == KEY_ESC)
        GetGazette()->hideCenter();
}

void CenterSurface::colorSchemeEvent(const CZColorSchemeEvent &e)
{
    MLayerSurface::colorSchemeEvent(e);
    updateState();
}

void CenterSurface::updateShape() noexcept
{
    // The panel (layout coordinates: right edge of the surface)
    const SkScalar width { layout().calculatedWidth() };
    const SkRect rect { SkRect::MakeXYWH(width - Margin + m_slide - PanelWidth, Margin,
                                         PanelWidth, std::max(0.f, layout().calculatedHeight() - 2 * Margin)) };
    SkPath shape;
    shape.addRRect(SkRRect::MakeRectXY(rect, PanelRadius, PanelRadius));

    if (shape != m_shape)
    {
        m_shape = shape;
        setBlurMaskPath(&m_shape);
    }
}

void CenterSurface::render() noexcept
{
    layout().calculate();
    updateShape();
    MLayerSurface::render();
}
