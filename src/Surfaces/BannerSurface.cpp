#include <Surfaces/BannerSurface.h>
#include <Core/Gazette.h>
#include <CZ/Marco/MScreen.h>
#include <CZ/AK/AKApp.h>
#include <CZ/AK/AKScene.h>
#include <CZ/Core/CZEase.h>
#include <CZ/Core/Events/CZPointerMoveEvent.h>
#include <CZ/Core/Events/CZPointerLeaveEvent.h>
#include <CZ/Core/Events/CZPointerEnterEvent.h>
#include <CZ/Core/Events/CZPointerButtonEvent.h>
#include <CZ/Core/Events/CZPointerScrollEvent.h>
#include <CZ/skia/core/SkRRect.h>
#include <linux/input-event-codes.h>
#include <algorithm>
#include <cmath>

// Room around the cards for their shadows (the top is the gap below the top bar)
static constexpr SkScalar MarginTop { 8.f };
static constexpr SkScalar MarginRight { 12.f };
static constexpr SkScalar MarginSide { 28.f };
static constexpr SkScalar Gap { 10.f };
static constexpr UInt32 HoverTimeoutMs { 2500 }; // Left after being hovered
static constexpr SkScalar DragThreshold { 4.f };  // Before a press becomes a drag (not a click)

// Rect of @p node in @p root's coordinates (from the layout, before rendering)
static SkRect RectIn(const AKNode &node, const AKNode &root) noexcept
{
    SkRect rect { SkRect::MakeXYWH(node.layout().calculatedLeft(), node.layout().calculatedTop(),
                                   node.layout().calculatedWidth(), node.layout().calculatedHeight()) };

    for (const AKNode *p { node.parent() }; p && p != &root; p = p->parent())
        rect.offset(p->layout().calculatedLeft(), p->layout().calculatedTop());

    return rect;
}

BannerSurface::BannerSurface(MScreen *screen) noexcept : m_screen(screen)
{
    setColor(CZAdaptiveColor(0));
    setBlurMaterial(MBlurMaterial::Popover);
    setBorderRadius(CZBorderRadius::Make(0));
    setScope("cz-gazette-banners");
    setLayer(Layer::Overlay);
    setKeyboardInteractivity(KeyboardInteractivity::None);
    setAnchor(CZEdgeTop | CZEdgeRight);
    setExclusiveZone(0); // Below the top bar

    layout().setWidth(Distance());
    m_column.layout().setFlexDirection(YGFlexDirectionColumn);
    m_column.layout().setAlignItems(YGAlignFlexEnd);
    m_column.layout().setPadding(YGEdgeTop, MarginTop);
    m_column.layout().setPadding(YGEdgeRight, MarginRight);
    m_column.layout().setPadding(YGEdgeLeft, MarginSide);
    m_column.layout().setPadding(YGEdgeBottom, MarginSide);
    m_column.layout().setGap(YGGutterRow, Gap);
    m_column.layout().setWidthPercent(100.f);
}

SkScalar BannerSurface::Distance() noexcept
{
    // Out of the surface (it ends at the screen's edge)
    return NotificationCard::Width + MarginRight + MarginSide;
}

/* ----------------------------------------- Banners ----------------------------------------- */

void BannerSurface::show(std::shared_ptr<Notification> notification) noexcept
{
    if (!notification || m_retired)
        return;

    if (!mapped())
    {
        setScreen(m_screen);
        setMapped(true);
    }

    auto banner { std::make_unique<Banner>() };
    Banner *b { banner.get() };
    b->card = std::make_unique<NotificationCard>(notification, NotificationCard::Style::Banner, &m_column);

    // At the top
    if (!m_banners.empty())
        b->card->insertBefore(m_banners.front()->card.get());

    const UInt32 id { notification->id };
    b->anim.onUpdate.subscribe(this, [this, b](CZAnimation *a){
        const SkScalar t { SkScalar(CZEase::OutCubic(a->value())) };
        setSlide(*b, b->from + (b->to - b->from) * t);
    });
    b->anim.onFinish.subscribe(this, [this, b, id](CZAnimation*){
        // Slid out: destroyed (later, it's its own animation)
        if (b->leaving)
            CZTimer::OneShot(0, [this, id](CZTimer*){
                for (auto &banner : m_banners)
                    if (banner->leaving && banner->card->notification()->id == id)
                        return destroy(banner.get());
            });
    });

    // Slides in from the screen's edge
    setSlide(*b, Distance());
    slideTo(*b, 0.f, ShowMs);

    // Stays if critical or if asked (expire_timeout 0)
    const Notification &n { *notification };

    if (n.urgency != Notification::Urgency::Critical && n.timeoutMs != 0)
        b->timeoutMs = n.timeoutMs > 0 ? UInt32(n.timeoutMs) : DefaultTimeoutMs;

    // Later: it may be destroyed (closed if transient)
    b->timer.setCallback([this, id](CZTimer*){
        CZTimer::OneShot(0, [this, id](CZTimer*){
            for (auto &banner : m_banners)
                if (banner->card->notification()->id == id)
                    leave(*banner);

            // Transient ones are closed (their banner keeps sliding out, see remove())
            GetGazette()->bannerExpired(id);
        });
    });

    m_banners.push_front(std::move(banner));

    if (!m_hovered && !m_drag.banner)
        startTimer(*b, b->timeoutMs);

    // Too many: the oldest ones go
    size_t shown { 0 };

    for (auto &banner : m_banners)
        if (!banner->leaving && ++shown > MaxBanners)
            leave(*banner);

    update();
}

void BannerSurface::retire() noexcept
{
    m_retired = true;
    m_drag = {};
    m_hovered = false;
    m_screen = nullptr;
    m_banners.clear();
    setMapped(false);
}

void BannerSurface::replace(std::shared_ptr<Notification> notification) noexcept
{
    for (auto &banner : m_banners)
    {
        if (banner->leaving || banner->card->notification()->id != notification->id)
            continue;

        banner->card->setNotification(notification);

        if (!m_hovered && !m_drag.banner)
            startTimer(*banner, banner->timeoutMs);

        update();
        return;
    }
}

void BannerSurface::remove(UInt32 id) noexcept
{
    for (auto &banner : m_banners)
    {
        if (banner->card->notification()->id == id)
        {
            leave(*banner);
            return;
        }
    }
}

void BannerSurface::hideAll() noexcept
{
    for (auto &banner : m_banners)
        leave(*banner);
}

void BannerSurface::slideTo(Banner &banner, SkScalar to, UInt32 ms) noexcept
{
    banner.from = banner.card->slide;
    banner.to = to;
    banner.anim.pause(); // Stopped to change its duration (ignored while running)
    banner.anim.setDuration(ms);
    banner.anim.reset(); // (Re)starts from where it is
    banner.anim.start();
}

void BannerSurface::setSlide(Banner &banner, SkScalar slide) noexcept
{
    banner.card->slide = slide;
    banner.card->layout().setPosition(YGEdgeLeft, slide);
    update();
}

void BannerSurface::startTimer(Banner &banner, UInt32 ms) noexcept
{
    banner.timer.stop();

    if (ms > 0 && banner.timeoutMs > 0)
        banner.timer.start(ms);
}

void BannerSurface::restartTimers(UInt32 ms) noexcept
{
    for (auto &banner : m_banners)
        if (!banner->leaving)
            startTimer(*banner, ms);
}

void BannerSurface::leave(Banner &banner) noexcept
{
    if (banner.leaving)
        return;

    if (m_drag.banner == &banner)
        m_drag = {};

    banner.leaving = true;
    banner.timer.stop();
    banner.card->setHovered(false);
    slideTo(banner, Distance(), HideMs);
}

void BannerSurface::destroy(Banner *banner) noexcept
{
    const auto it { std::find_if(m_banners.begin(), m_banners.end(), [banner](const auto &b){ return b.get() == banner; }) };

    if (it == m_banners.end())
        return;

    if (m_drag.banner == banner)
        m_drag = {};

    m_banners.erase(it);

    if (m_banners.empty())
    {
        m_hovered = false;
        setMapped(false);
    }
    else
        update();
}

BannerSurface::Banner *BannerSurface::bannerAt(SkPoint pos, bool leaving) noexcept
{
    for (auto &banner : m_banners)
        if ((leaving || !banner->leaving) && SkRect::Make(banner->card->worldRect()).contains(pos.x(), pos.y()))
            return banner.get();

    return nullptr;
}

/* ------------------------------------------ Drags ------------------------------------------ */

void BannerSurface::dragBegin(Banner &banner, SkScalar x, bool fingers) noexcept
{
    m_drag = {};
    m_drag.banner = &banner;
    m_drag.startX = x;
    m_drag.startSlide = banner.card->slide;
    m_drag.fingers = fingers;
    m_drag.last = std::chrono::steady_clock::now();
    m_drag.wasLeaving = banner.leaving;

    // Sliding (in, or out by itself): it stops where it is, the drag starts from there
    banner.anim.pause();
    banner.leaving = false;
    m_drag.startSlide = banner.card->slide;

    // None expires meanwhile
    for (auto &b : m_banners)
        b->timer.stop();
}

void BannerSurface::dragUpdate(SkScalar offset) noexcept
{
    Banner *b { m_drag.banner };

    if (!b)
        return;

    const auto now { std::chrono::steady_clock::now() };
    const SkScalar ms { std::max(1.f, std::chrono::duration<SkScalar, std::milli>(now - m_drag.last).count()) };
    const SkScalar prev { m_drag.offset };
    m_drag.offset = offset;
    m_drag.last = now;

    // Smoothed, to the right
    m_drag.velocity = 0.6f * m_drag.velocity + 0.4f * ((offset - prev) / ms);

    if (!m_drag.moved)
    {
        if (std::fabs(offset) < DragThreshold)
            return;

        // A drag, not a click
        m_drag.moved = true;
        b->card->cancelPress();
    }

    // Only to the right (resisting a bit to the left)
    SkScalar slide { m_drag.startSlide + offset };

    if (slide < 0.f)
        slide *= 0.15f;

    setSlide(*b, std::min(slide, Distance()));
}

void BannerSurface::dragEnd() noexcept
{
    Banner *b { m_drag.banner };
    const Drag drag { m_drag };
    m_drag = {};

    if (!b)
        return;

    if (drag.moved)
    {
        const bool dismiss { b->card->slide > NotificationCard::Width * DismissFraction || drag.velocity > DismissVelocity };

        if (dismiss)
        {
            // Hidden (still in the center; transient ones are closed)
            const UInt32 id { b->card->notification()->id };
            leave(*b);
            CZTimer::OneShot(0, [id](CZTimer*){ GetGazette()->bannerDismissed(id); });
        }
        else
            slideTo(*b, 0.f, HideMs);
    }

    // Not dragged (a click or a tap): what it was doing goes on from where it stopped
    else if (drag.wasLeaving)
        leave(*b);
    else if (b->card->slide != 0.f)
        slideTo(*b, 0.f, HideMs);

    if (!m_hovered)
        restartTimers(HoverTimeoutMs);
}

/* ------------------------------------------ Input ------------------------------------------ */

void BannerSurface::pointerButtonEvent(const CZPointerButtonEvent &e)
{
    const SkPoint pos { AKApp::Get()->pointer().pos() };

    if (e.button == BTN_LEFT && e.pressed)
    {
        if (Banner *b { bannerAt(pos, true) })
            dragBegin(*b, pos.x(), false);
    }
    else if (e.button == BTN_LEFT && m_drag.banner && !m_drag.fingers)
    {
        // A drag: no click (the card handles the release after this)
        if (m_drag.moved)
            m_drag.banner->card->cancelPress();

        MLayerSurface::pointerButtonEvent(e);
        dragEnd();
        return;
    }

    MLayerSurface::pointerButtonEvent(e);
}

void BannerSurface::pointerMoveEvent(const CZPointerMoveEvent &e)
{
    MLayerSurface::pointerMoveEvent(e);

    if (m_drag.banner && !m_drag.fingers)
        dragUpdate(AKApp::Get()->pointer().pos().x() - m_drag.startX);

    updateHover();
}

void BannerSurface::pointerScrollEvent(const CZPointerScrollEvent &e)
{
    MLayerSurface::pointerScrollEvent(e);

    // Two finger swipes (touchpad)
    if (e.source != CZPointerScrollEvent::Finger || !e.hasX)
        return;

    // Lifted
    if (e.axes.x() == 0.f)
    {
        if (m_drag.banner && m_drag.fingers)
            dragEnd();

        return;
    }

    if (!m_drag.banner)
    {
        Banner *b { bannerAt(AKApp::Get()->pointer().pos(), true) };

        if (!b)
            return;

        dragBegin(*b, 0.f, true);
    }

    if (!m_drag.fingers)
        return;

    // The fingers' motion (natural scrolling inverts the axis)
    const SkScalar dx { e.relativeDirectionX == CZPointerScrollEvent::Inverted ? -e.axes.x() : e.axes.x() };
    dragUpdate(m_drag.offset + dx);
}

void BannerSurface::pointerLeaveEvent(const CZPointerLeaveEvent &e)
{
    MLayerSurface::pointerLeaveEvent(e);

    // Left while dragging: the release may never arrive here (e.g. released past the surface's
    // edge), so the drag ends now (else it'd keep following the pointer once it's back)
    if (m_drag.banner)
        dragEnd();

    for (auto &banner : m_banners)
        banner->card->setHovered(false);

    if (m_hovered)
    {
        m_hovered = false;

        // They stay a bit longer
        if (!m_drag.banner)
            restartTimers(HoverTimeoutMs);
    }
}

void BannerSurface::pointerEnterEvent(const CZPointerEnterEvent &e)
{
    MLayerSurface::pointerEnterEvent(e);

    // Back without a release seen: whatever was dragged is let go
    if (m_drag.banner && !m_drag.fingers)
        dragEnd();
}

void BannerSurface::updateHover() noexcept
{
    const SkPoint pos { AKApp::Get()->pointer().pos() };
    Banner *hovered { m_drag.banner ? m_drag.banner : bannerAt(pos) };

    for (auto &banner : m_banners)
        banner->card->setHovered(banner.get() == hovered);

    // Hovered: none expires meanwhile
    if (hovered && !m_hovered)
    {
        m_hovered = true;

        for (auto &banner : m_banners)
            banner->timer.stop();
    }
}

/* ------------------------------------------ Shape ------------------------------------------ */

void BannerSurface::updateShape() noexcept
{
    SkPath shape;
    SkRegion input;

    for (auto &banner : m_banners)
    {
        const SkRect rect { RectIn(*banner->card, *this) };
        shape.addRRect(SkRRect::MakeRectXY(rect, NotificationCard::Radius, NotificationCard::Radius));

        input.op(rect.round(), SkRegion::kUnion_Op);
    }

    // Dragged: input everywhere (the pointer may go past the card)
    if (m_drag.banner && m_drag.moved)
        input.setRect(SkIRect::MakeWH(Int32(layout().calculatedWidth()), Int32(layout().calculatedHeight())));

    if (shape != m_shape)
    {
        m_shape = shape;
        setBlurMaskPath(&m_shape);
    }

    if (input != m_input)
    {
        m_input = input;
        setCustomInputRegion(&m_input);
    }
}

void BannerSurface::render() noexcept
{
    layout().calculate();
    updateShape();
    MLayerSurface::render();
}
