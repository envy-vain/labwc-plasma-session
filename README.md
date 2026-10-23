# labwc-plasma-session

A lightweight **LXQt + labwc** Wayland session that *feels like KDE Plasma 6*: the same shortcuts and virtual
desktops, natural touchpad scrolling, 3-finger swipes, **live pinch zoom**, a centered volume/brightness OSD,
and Plasma-style Wi-Fi / Bluetooth / battery / volume popups in the tray. Install it next to Plasma and pick it on
the login screen when you want your battery to last.

On my ThinkPad L13 (i7-1165G7), the whole session uses **~1.7 GB of RAM** and the laptop idles at
**~3.7 W** for the entire machine.

## Install

```sh
git clone https://github.com/envy-vain/labwc-plasma-session
cd labwc-plasma-session
./install.sh
```

Then log out and choose **LXQt (Wayland)** on the login screen. The installer:

- installs the packages (Arch / CachyOS / EndeavourOS / Manjaro, Fedora, Debian testing and recent Ubuntu), asking
  for sudo. Other distros: install the [package list](#packages) yourself, then run `./install.sh --no-deps`;
- backs up every file it replaces (`~/.local/state/labwc-plasma-session/backup`), so `./uninstall.sh` restores your old setup;
- puts the scripts in `~/.local/bin`, builds `quickpanel` and `panelstat` from source, writes the labwc and LXQt
  configs, and sets labwc as LXQt's compositor;
- adds you to the `input` group so the gesture daemon can read touchpad events (see [Notes](#notes)).

It's safe to re-run (e.g. after `git pull`): configs are replaced with the repo's versions again, and any you
changed are kept beside them as `<file>.before-update-<time>`. `./install.sh --labwc-patch` also builds labwc with a close button on the Alt+Tab
thumbnails (Arch-based distros only).

## What you get

| | |
|---|---|
| **Plasma shortcuts** | Meta opens the menu, Meta+D shows the desktop, Meta+PgUp/PgDn maximize/minimize, Meta+arrows snap, Meta+Shift+arrows move to the next screen, Alt+F3 window menu, Meta+Ctrl+Esc kills a window, Meta+W/G list all windows |
| **4 desktops, 2×2 grid like Plasma** | Meta+Ctrl+arrows (wrap around), Meta+Ctrl+Shift+arrows take the window along, Ctrl/Meta+F1–F4, scroll on the desktop |
| **Touchpad** | natural scrolling, tap to click, **3-finger swipe** = switch desktop, **3-finger pinch** = live zoom that follows your fingers (or Meta+= / Meta+-; Meta+0 turns the zoom on/off) |
| **Centered OSD** | volume and brightness keys (Shift = 1% steps; at 10% brightness and below, always 1% steps), mic mute, volume up to 150% like Plasma, charger plugged/unplugged, power profile (Meta+B cycles it) |
| **quickpanel** | native tray icons and popups for Wi-Fi (with a speed test against Cloudflare, only when you press it; it can use a few hundred MB on a fast line), Bluetooth (scan and pair right in the popup: headphones, speakers and other no-PIN devices; keyboards and phones that show a code pair through your Bluetooth settings), battery (power profiles, charge limit) and volume (the battery icon only appears if there is a battery). One small resident Qt process, event-driven, no polling |
| **Panel** | CPU temperature and live wattage (whole laptop + CPU package) from `panelstat`, a tiny C helper so the 5-second refresh costs almost nothing |
| **Screen off** | after 3 minutes idle, but never while a video or music is playing |
| **Screenshots** | Print = whole screen, Shift+Print or Meta+Shift+S = select an area; saved to ~/Pictures/Screenshots and copied to the clipboard |
| **Monitors** | arrange them in `wdisplays`, then run *Save Monitor Layout*: `kanshi` restores it on login and when you plug a screen in |
| **Alt+F2** | the LXQt runner starts on demand and quits 45 s after you last used it, instead of sitting in RAM |

### Works well with

- **[plasma-themer](https://github.com/envy-vain/plasma-themer)**: one-key theme switching for Plasma that also
  themes this session (panel, menus, window borders, wallpaper). Same keys: Meta+Ctrl+Shift+Space opens the picker.
- **[ultra-low-power](https://github.com/envy-vain/thinkpad-l13-linux#ultra-low-power-mode)**: a 3 W CPU cap mode.
  When it's installed, the panel gets an *Eco* button, quickpanel's battery popup gets a toggle and Meta+Shift+U switches it.

## Packages

What the installer installs, for doing it by hand:

- **Session:** labwc (0.20 or newer), lxqt-wayland-session, lxqt-session, lxqt-panel, lxqt-runner, lxqt-config,
  lxqt-powermanagement, lxqt-qtplugin, lxqt-policykit, lxqt-notificationd, lxqt-themes, pcmanfm-qt
- **Tools:** swaybg, swayidle, swayosd (optional: without it you get notifications instead of the OSD), wlopm,
  playerctl, wtype, grim, slurp, wl-clipboard, brightnessctl, kanshi, wlr-randr, wdisplays, jq, notify-send
  (libnotify), `libinput debug-events` (libinput-tools / libinput-utils), wireplumber (`wpctl`), pavucontrol-qt and
  nm-connection-editor (the settings buttons in the popups), python3
- **To build quickpanel:** cmake, a C++20 compiler, Qt 6 base (Widgets, Network, DBus), KF6 NetworkManagerQt, libpulse

quickpanel talks to NetworkManager, BlueZ, UPower, power-profiles-daemon (or tuned-ppd) and PipeWire/PulseAudio over
D-Bus. Most laptop distros already run all of them.

## Files

| Path | What it is |
|---|---|
| `config/labwc/rc.xml` | the stock LXQt labwc config + the Plasma keybinds, desktops, touchpad, magnifier (changes marked "Plasma") |
| `config/labwc/autostart` | wallpaper, idle, OSD, gestures, quickpanel, kanshi |
| `config/lxqt/panel.conf` | bottom panel, 44 px, Plasma-like layout |
| `bin/touchpad-gestures` | swipes + live pinch zoom from `libinput debug-events` |
| `bin/fnkey`, `bin/power-osd` | volume/brightness/profile keys and charger/profile OSD |
| `bin/idle-screen-off`, `bin/screenshot`, `bin/zoom-smooth`, `bin/lxqt-runner-toggle`, `bin/save-monitor-layout` | the small helpers described above |
| `src/quickpanel/` | the tray popups (C++ / Qt 6) |
| `src/panelstat/` | panel temperature / wattage (C) |
| `patches/labwc/` | Alt+Tab close button: an × on each thumbnail, and Delete closes the selected window |

**Your own wallpaper:** put an image (or a link to one) at `~/.config/labwc/wallpaper`. Otherwise you get the
plasma-themer wallpaper, or a plain dark background.

## Notes

- **The `input` group** lets the gesture daemon read the touchpad. It also lets any program you run as yourself
  read keyboard input, which is the usual trade-off for gesture tools (libinput-gestures needs the same).
  To leave it: `sudo gpasswd -d $USER input`, then remove `touchpad-gestures` from `~/.config/labwc/autostart`.
- **Super+Enter opens Konsole** and the Bluetooth settings button opens KDE's settings module. Both assume Plasma is
  also installed, which is the point of this session. Change them in `rc.xml` / `menu.xml` if it isn't.
- labwc has no blur, overview or tiling editor, so those Plasma features aren't here. Meta+W lists all windows instead.
- **Patched labwc:** after an upgrade replaces it, a pacman hook reminds you to run `patches/labwc/rebuild.sh` again.
- Tested on CachyOS with labwc 0.20.2 and LXQt 2.4. The Fedora and Debian package lists were checked against the
  repositories, but I haven't run the full installer on those distros yet. Issues and PRs are welcome.

## Undo

```sh
./uninstall.sh
```

It puts back every file the installer replaced and deletes the ones it created. Packages stay installed.

## Credits

Made by [envy-vain](https://github.com/envy-vain).

**LLM disclaimer:** much of the code in this repo was written with help from an LLM (Claude, by Anthropic). I
directed it, tested every piece on my own laptop, and use this session every day. Read scripts before you
run them, as with anything from the internet.

The labwc patch is GPL-2.0-only, like labwc itself. Everything else is [MIT](LICENSE).
