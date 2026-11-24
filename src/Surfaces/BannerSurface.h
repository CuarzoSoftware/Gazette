#ifndef BANNERSURFACE_H
#define BANNERSURFACE_H

#include <Nodes/NotificationCard.h>
#include <CZ/Marco/Roles/MLayerSurface.h>
#include <CZ/Marco/Nodes/MVibrancyView.h>
#include <CZ/Core/CZTimer.h>
#include <CZ/Core/CZLinearAnimation.h>
#include <CZ/skia/core/SkPath.h>
#include <chrono>
#include <list>
#include <memory>

/**
 * @brief New notifications as banners at the top right of the default screen (like macOS).
 *
 * A layer surface (overlay layer, below the top bar's exclusive zone) with a column of cards,
 * newest at the top, at most MaxBanners. They slide in from the right edge of the screen and out
 * to it when they expire, after a few seconds (or expire_timeout, never if it's 0 or the
 * notification is critical). Hovering them keeps them. Only the cards are blurred and take input;
 * the surface is unmapped while there are none.
 *
 * Dragging a banner to the right (with the pointer or swiping two fingers on a touchpad) hides it
 * (it stays in the notification center); released before DismissFraction of its width (and
 * slowly), it slides back. Grabbed while sliding (in, or out by itself), it stops where it is and
 * the drag starts from there.
 */
class BannerSurface final : public MLayerSurface
{
public:
    static constexpr size_t MaxBanners { 3 };           ///< Shown at once (the oldest leaves for a new one)
    static constexpr UInt32 DefaultTimeoutMs { 5000 };  ///< How long a banner stays (expire_timeout -1)
    static constexpr UInt32 ShowMs { 380 }; ///< Sliding in
    static constexpr UInt32 HideMs { 220 }; ///< Sliding out or back (ease-out: moves right away)
    static constexpr SkScalar DismissFraction { 0.3f };  ///< Dragged this far (of its width) to the right: hidden
    static constexpr SkScalar DismissVelocity { 0.6f };  ///< Or released this fast (logical px per ms, a flick)

    BannerSurface() noexcept;

    /// Shows a banner for @p notification (sliding in, at the top).
    void show(std::shared_ptr<Notification> notification) noexcept;

    /// Updates its banner if shown (a notification replaced, same id).
    void replace(std::shared_ptr<Notification> notification) noexcept;

    /// Slides its banner out (if shown), it's destroyed once out (e.g. the notification was closed or expired).
    void remove(UInt32 id) noexcept;

    /// Slides every banner out (e.g. the notification center was opened).
    void hideAll() noexcept;

protected:
    void render() noexcept override;
    void pointerMoveEvent(const CZPointerMoveEvent &e) override;
    void pointerLeaveEvent(const CZPointerLeaveEvent &e) override;
    void pointerEnterEvent(const CZPointerEnterEvent &e) override;
    void pointerButtonEvent(const CZPointerButtonEvent &e) override;
    void pointerScrollEvent(const CZPointerScrollEvent &e) override;

    /// Sized by the cards (the configured size is the one requested, it'd fix the height)
    void suggestedSizeChanged() override {}

private:
    struct Banner
    {
        std::unique_ptr<NotificationCard> card;
        CZTimer timer;
        CZLinearAnimation anim;
        SkScalar from { 0.f }, to { 0.f };
        bool leaving { false };
        UInt32 timeoutMs { 0 }; // 0: stays
    };

    // A banner being dragged (pointer or touchpad)
    struct Drag
    {
        Banner *banner { nullptr };
        SkScalar startX { 0.f }, startSlide { 0.f }, offset { 0.f };
        SkScalar velocity { 0.f }; // Px per ms (to the right)
        std::chrono::steady_clock::time_point last;
        bool moved { false };
        bool fingers { false };
        bool wasLeaving { false }; // Grabbed while sliding out by itself
    };

    static SkScalar Distance() noexcept;
    Banner *bannerAt(SkPoint pos, bool leaving = false) noexcept;
    void slideTo(Banner &banner, SkScalar to, UInt32 ms) noexcept;
    void setSlide(Banner &banner, SkScalar slide) noexcept;
    void startTimer(Banner &banner, UInt32 ms) noexcept;
    void leave(Banner &banner) noexcept;
    void destroy(Banner *banner) noexcept;
    void dragBegin(Banner &banner, SkScalar x, bool fingers) noexcept;
    void dragUpdate(SkScalar offset) noexcept;
    void dragEnd() noexcept;
    void restartTimers(UInt32 ms) noexcept;
    void updateHover() noexcept;
    void updateShape() noexcept;
    MVibrancyView m_column { this }; // The cards (blurred, masked to them, see updateShape())
    std::list<std::unique_ptr<Banner>> m_banners; // Newest first
    Drag m_drag;
    SkPath m_shape;
    SkRegion m_input;
    bool m_hovered { false };
};

#endif // BANNERSURFACE_H
