#ifndef CENTERSURFACE_H
#define CENTERSURFACE_H

#include <Nodes/NotificationCard.h>
#include <CZ/Marco/Roles/MLayerSurface.h>
#include <CZ/Marco/Nodes/MVibrancyView.h>
#include <CZ/AK/Nodes/AKContainer.h>
#include <CZ/AK/Nodes/AKScroll.h>
#include <CZ/AK/Nodes/AKSwitch.h>
#include <CZ/AK/Nodes/AKText.h>
#include <CZ/AK/Nodes/AKButton.h>
#include <CZ/AK/Nodes/AKFontIcon.h>
#include <CZ/AK/Nodes/AKRoundSolidColor.h>
#include <CZ/Core/CZTimer.h>
#include <CZ/Core/CZLinearAnimation.h>
#include <CZ/skia/core/SkPath.h>
#include <list>
#include <memory>
#include <vector>

/**
 * @brief The notification center: a rounded panel sliding in from the right edge of the screen,
 *        over a subtle blur.
 *
 * From top to bottom: the title and Clear All, the Do Not Disturb toggle, and every notification
 * (newest first, scrollable). A layer surface (overlay layer, below the top bar's exclusive zone)
 * covering the screen while shown: only the panel is drawn (and blurred), clicking elsewhere or
 * pressing Escape hides it.
 */
class CenterSurface final : public MLayerSurface
{
public:
    static constexpr UInt32 ShowMs { 320 }; ///< Sliding in
    static constexpr UInt32 HideMs { 220 }; ///< Ease-out too: it moves right away

    CenterSurface() noexcept;

    /// Slides in on @p screen (nullptr: the compositor chooses; Gazette passes screenNamed()).
    void show(MScreen *screen) noexcept;

    /// Slides out.
    void hide() noexcept;

    /// Shown or showing (false while sliding out).
    bool shown() const noexcept { return m_shown; }

protected:
    void render() noexcept override;
    void pointerButtonEvent(const CZPointerButtonEvent &e) override;
    void pointerMoveEvent(const CZPointerMoveEvent &e) override;
    void pointerLeaveEvent(const CZPointerLeaveEvent &e) override;
    void keyboardKeyEvent(const CZKeyboardKeyEvent &e) override;
    void colorSchemeEvent(const CZColorSchemeEvent &e) override;

private:
    // A rounded translucent group (the Do Not Disturb row)
    class Card final : public AKRoundSolidColor
    {
    public:
        Card(AKNode *parent) noexcept;
    protected:
        void colorSchemeEvent(const CZColorSchemeEvent &e) override;
    private:
        void updateColors() noexcept;
    };

    void rebuild() noexcept;
    void updateState() noexcept;
    void updateShape() noexcept;
    void updateHover() noexcept;
    void setSlide(SkScalar slide) noexcept;
    void slideTo(SkScalar to, UInt32 ms) noexcept;

    MVibrancyView m_panel { this }; // Blurred (masked to its rounded rect, see updateShape())
    AKContainer m_header { YGFlexDirectionRow, false, &m_panel };
    AKText m_title { "Notifications", &m_header };
    AKContainer m_headerSpacer { YGFlexDirectionRow, false, &m_header };
    AKButton m_clear { "Clear All", &m_header };
    Card m_dndCard { &m_panel };
    AKRoundSolidColor m_dndBadge { 0, CZBorderRadius::Make(15), &m_dndCard };
    AKFontIcon m_dndIcon { "bedtime", 18, &m_dndBadge };
    AKContainer m_dndTexts { YGFlexDirectionColumn, false, &m_dndCard };
    AKText m_dndTitle { "Do Not Disturb", &m_dndTexts };
    AKText m_dndSubtitle { "", &m_dndTexts };
    AKSwitch m_dndSwitch { false, &m_dndCard };
    AKScroll m_scroll { &m_panel };
    AKContainer m_list { YGFlexDirectionColumn, false, &m_scroll };
    AKText m_empty { "No Notifications", &m_panel };
    std::vector<std::unique_ptr<NotificationCard>> m_cards; // Newest first

    // A closed notification's card leaving the list: the inverse of Adding, it goes up fading out
    // while its room closes, pulling the older ones up (see removeCard())
    struct Removing
    {
        std::unique_ptr<NotificationCard> card;
        CZLinearAnimation anim;
        SkScalar room { 1.f }, opacity { 1.f }; // Where it starts (removed while still entering)
        bool collapse { true }; // False: just fades out in place (all of them cleared at once)
    };
    std::list<std::unique_ptr<Removing>> m_removing;
    void removeCard(std::unique_ptr<NotificationCard> card, bool collapse) noexcept;

    // A new notification's card entering the list (at the top): its room opens, pushing the
    // others down, while it comes down from above fading in (see addCard())
    struct Adding
    {
        NotificationCard *card;
        CZLinearAnimation anim;
        SkScalar room { 0.f }, opacity { 0.f }; // Current (see Removing)
    };
    std::list<std::unique_ptr<Adding>> m_adding;
    void addCard(NotificationCard *card) noexcept;
    void finishAdding(NotificationCard *card) noexcept;
    void updateEmpty() noexcept;

    CZLinearAnimation m_anim;
    CZTimer m_clock;      // Updates the times while shown
    SkScalar m_slide { 0.f };
    SkScalar m_slideFrom { 0.f }, m_slideTo { 0.f };
    bool m_shown { false };
    bool m_pressedOutside { false };
    SkPath m_shape;
};

#endif // CENTERSURFACE_H
