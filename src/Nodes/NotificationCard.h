#ifndef NOTIFICATIONCARD_H
#define NOTIFICATIONCARD_H

#include <Core/Notification.h>
#include <CZ/AK/Nodes/AKRoundSolidColor.h>
#include <CZ/AK/Nodes/AKContainer.h>
#include <CZ/AK/Nodes/AKImageFrame.h>
#include <CZ/AK/Nodes/AKText.h>
#include <CZ/AK/Nodes/AKButton.h>
#include <CZ/AK/Nodes/AKFontIcon.h>
#include <CZ/AK/Effects/AKBackgroundBoxShadowEffect.h>
#include <functional>
#include <memory>
#include <vector>

/**
 * @brief A notification: the app's icon, name and time, the summary (bold), the body (a few lines,
 *        wrapped) and its image at the right, then its action buttons.
 *
 * Clicking it invokes its "default" action (or just closes it). While hovered (see setHovered()),
 * a close button shows over its top left corner (list style only: banners are dragged away).
 *
 * - Banner style: transparent over the surface's blur (see BannerSurface), with a soft shadow.
 * - List style: a translucent card over the notification center's panel (see CenterSurface).
 */
class NotificationCard final : public AKRoundSolidColor
{
public:
    enum class Style { Banner, List };

    static constexpr SkScalar Width { 344.f };
    static constexpr Int32 Radius { 16 };

    NotificationCard(std::shared_ptr<Notification> notification, Style style, AKNode *parent = nullptr) noexcept;

    /// Shows another notification (the same id replaced).
    void setNotification(std::shared_ptr<Notification> notification) noexcept;
    const std::shared_ptr<Notification> &notification() const noexcept { return m_notification; }

    void setHovered(bool hovered) noexcept;
    bool hovered() const noexcept { return m_hovered; }

    /// Updates the time text ("5m ago").
    void updateTime() noexcept;

    /// Forgets a press (it became a drag, see BannerSurface): its release won't click.
    void cancelPress() noexcept;

    /// Over the top left corner (list style): how far it goes out of the card.
    static constexpr SkScalar CloseOverhang { 7.f };

    /// Horizontal offset while sliding (banners, see BannerSurface), logical units.
    SkScalar slide { 0.f };

protected:
    void pointerButtonEvent(const CZPointerButtonEvent &e) override;
    void colorSchemeEvent(const CZColorSchemeEvent &e) override;

private:
    // A round × (a solid circle with a soft shadow)
    class CloseButton final : public AKRoundSolidColor
    {
    public:
        static constexpr SkScalar Size { 20.f };
        CloseButton(AKNode *parent) noexcept;
        std::function<void()> onClicked;
    protected:
        void pointerEnterEvent(const CZPointerEnterEvent &e) override;
        void pointerLeaveEvent(const CZPointerLeaveEvent &e) override;
        void pointerButtonEvent(const CZPointerButtonEvent &e) override;
        void colorSchemeEvent(const CZColorSchemeEvent &e) override;
    private:
        void updateColors() noexcept;
        AKBackgroundBoxShadowEffect m_shadow;
        AKFontIcon m_icon { "close", 13, this };
        bool m_hover { false }, m_pressed { false };
    };

    void updateColors() noexcept;
    std::shared_ptr<Notification> m_notification;
    Style m_style;
    bool m_hovered { false }, m_pressed { false };
    AKBackgroundBoxShadowEffect m_shadow;
    AKContainer m_row { YGFlexDirectionRow, false, this };
    AKImageFrame m_icon { &m_row };
    AKContainer m_texts { YGFlexDirectionColumn, false, &m_row };
    AKContainer m_header { YGFlexDirectionRow, false, &m_texts };
    AKText m_app { "", &m_header };
    AKContainer m_headerSpacer { YGFlexDirectionRow, false, &m_header };
    AKText m_time { "", &m_header };
    AKText m_summary { "", &m_texts };
    AKText m_body { "", &m_texts };
    AKImageFrame m_image { &m_row };
    AKContainer m_actions { YGFlexDirectionRow, false, this };
    std::vector<std::unique_ptr<AKButton>> m_buttons;
    CloseButton m_close { this };
};

#endif // NOTIFICATIONCARD_H
