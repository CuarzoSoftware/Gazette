# Gazette

<p align="left">
  <a href="https://github.com/CuarzoSoftware/Gazette/blob/main/LICENSE">
    <img src="https://img.shields.io/badge/license-LGPLv2.1-blue.svg" alt="Gazette is released under the LGPLv2.1 license." />
  </a>
</p>

> This project is part of the [Cuarzo Framework](https://github.com/CuarzoSoftware/Cuarzo). Follow the instructions there to build and install it.

The notifications daemon of Cuarzo OS (`cz-gazette`), built on Marco and Kay, like macOS's notifications. Crystals launches it with the rest of the desktop.

- **Banners:** new notifications slide in at the top right of the screen, below the menu bar. Each shows the app's icon and name, the summary, a few lines of the body, an image if the app sends one, and up to three action buttons. They leave after 5 seconds, or after the app's `expire_timeout`. Critical ones and those with an `expire_timeout` of 0 stay until closed. Hovering them keeps them on screen.
- **Notification center:** slides in from the right edge of the screen. It has the Do Not Disturb toggle, Clear All, and every notification, newest first, in a scrollable list. Clicking outside it or pressing Escape hides it. Firmament's notifications button (at the right end of the menu bar) toggles it.
- **Do Not Disturb:** no banners. Notifications still go to the notification center. Saved in `~/.config/cz-gazette/gazette.conf`.
- Clicking a notification invokes its `default` action, or just dismisses it. Notifications stay in the notification center until they are closed, cleared, or one of their actions is invoked. Resident notifications stay after their actions too. Transient ones are never kept in the center.

## D-Bus

Gazette implements the [Desktop Notifications Specification](https://specifications.freedesktop.org/notification-spec/latest/) 1.2 (`org.freedesktop.Notifications` on the user bus): `Notify`, `CloseNotification`, `GetCapabilities`, `GetServerInformation`, and the `NotificationClosed` and `ActionInvoked` signals.

Both interfaces come from [Services](https://github.com/CuarzoSoftware/Services) (`SVNotificationsServer`); apps send notifications and control the center with its `SVNotificationsClient`.

- **Capabilities:** `actions`, `body`, `body-markup`, `icon-static` and `persistence`.
- **Markup:** bodies are shown as plain text, without their markup.
- **Hints:** `urgency`, `desktop-entry`, `image-data`, `image-path`, `transient` and `resident`.

The desktop uses `org.cuarzo.Gazette` at `/org/cuarzo/Gazette`:

| Member | Description |
| --- | --- |
| `ToggleCenter(s screen)`, `ShowCenter(s screen)` | The notification center on the screen with that name (e.g. `DP-1`), or the primary one if the name is empty or not found |
| `HideCenter()` | Hides the notification center |
| `SetDoNotDisturb(b enabled)` | Turns Do Not Disturb on or off |
| `GetState() → (b centerShown, b doNotDisturb, u count, s screen)` | The current state (`screen`: the name of the screen the center is shown on, empty if hidden) |
| `StateChanged(b centerShown, b doNotDisturb, u count, s screen)` | Signal: emitted when any of them changes |

```sh
notify-send -a Firefox -i firefox "Download complete" "report.pdf has finished downloading."
gdbus call --session -d org.cuarzo.Gazette -o /org/cuarzo/Gazette -m org.cuarzo.Gazette.ToggleCenter ""
```

## Building

It's built as part of the Cuarzo meta project (`with_gazette` option).

## License

Released under the GNU Lesser General Public License v2.1. See [LICENSE](LICENSE).
