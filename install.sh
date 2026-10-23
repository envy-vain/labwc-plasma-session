#!/usr/bin/env bash
# labwc-plasma-session installer. Safe to re-run; every file it replaces is backed up the first time.
#   ./install.sh                 packages (asks for sudo), then configs, scripts, quickpanel and panelstat
#   ./install.sh --no-deps       skip the packages (other distros: install the README's list first)
#   ./install.sh --labwc-patch   also rebuild labwc with the Alt+Tab close button (Arch-based distros only)
# Undo with ./uninstall.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
cfg=${XDG_CONFIG_HOME:-$HOME/.config}
bin=$HOME/.local/bin
state=${XDG_STATE_HOME:-$HOME/.local/state}/labwc-plasma-session
bak=$state/backup

say()  { printf '\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33m  ! %s\033[0m\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

deps=1 patch=
for a in "$@"; do
  case $a in
    --no-deps) deps= ;;
    --labwc-patch) patch=1 ;;
    -h | --help) sed -n '2,6s/^# \{0,1\}//p' "$0"; exit 0 ;;
    *) echo "unknown option: $a (try --help)" >&2; exit 2 ;;
  esac
done
[[ $EUID -ne 0 ]] || { echo "Run this as your normal user; it asks for sudo when it needs it." >&2; exit 1; }

. /etc/os-release 2>/dev/null || true
family=unknown
case " ${ID:-} ${ID_LIKE:-} " in
  *" arch "* | *" cachyos "* | *" endeavouros "* | *" manjaro "*) family=arch ;;
  *" fedora "* | *" rhel "*) family=fedora ;;
  *" debian "* | *" ubuntu "*) family=debian ;;
esac

# ------------------------------------------------------------------ packages
lxqt=(lxqt-wayland-session lxqt-session lxqt-panel lxqt-runner lxqt-config lxqt-powermanagement lxqt-qtplugin
  lxqt-policykit lxqt-notificationd lxqt-themes pcmanfm-qt)
tools=(labwc swaybg swayidle wlopm playerctl wtype grim slurp wl-clipboard brightnessctl kanshi wlr-randr wdisplays jq
  wireplumber pavucontrol-qt nm-connection-editor)
if [[ $deps ]]; then
  say "packages for ${PRETTY_NAME:-this distro}"
  case $family in
    arch)
      inst=(sudo pacman -S --needed --noconfirm)
      pkgs=("${lxqt[@]}" "${tools[@]}" swayosd libnotify libinput-tools python
        base-devel cmake pkgconf qt6-base networkmanager-qt libpulse) ;;
    fedora) # no swayosd package: fnkey/power-osd fall back to notifications
      inst=(sudo dnf install -y)
      pkgs=("${lxqt[@]}" "${tools[@]}" libnotify libinput-utils python3
        gcc-c++ cmake pkgconf-pkg-config qt6-qtbase-devel kf6-networkmanager-qt-devel pulseaudio-libs-devel) ;;
    debian)
      sudo apt-get update -qq
      inst=(sudo apt-get install -y)
      pkgs=("${lxqt[@]}" "${tools[@]}" swayosd libnotify-bin libinput-tools python3
        build-essential cmake pkgconf qt6-base-dev libkf6networkmanagerqt-dev libpulse-dev) ;;
    *)
      echo "Unknown distro. Install the packages listed in the README, then run: ./install.sh --no-deps" >&2
      exit 1 ;;
  esac
  if ! "${inst[@]}" "${pkgs[@]}"; then
    warn "installing everything at once failed; trying one package at a time"
    missing=()
    for p in "${pkgs[@]}"; do "${inst[@]}" "$p" >/dev/null || missing+=("$p"); done
    [[ ${#missing[@]} -eq 0 ]] || warn "failed to install (see the errors above): ${missing[*]}"
  fi
fi

for c in labwc lxqt-panel startlxqtwayland python3 cmake cc; do
  have "$c" || { echo "Missing $c. Install it (see the README) and run the installer again." >&2; exit 1; }
done
# Optional tools: each one missing turns off one feature, so say which
gone=()
for c in swaybg swayidle swayosd-client wlopm playerctl wtype grim slurp wl-copy brightnessctl kanshi wlr-randr \
  wdisplays jq notify-send wpctl powerprofilesctl libinput; do
  have "$c" || gone+=("$c")
done
[[ ${#gone[@]} -eq 0 ]] || warn "not found, the features that use them won't work: ${gone[*]}"
ver=$(labwc --version | grep -oE '[0-9]+\.[0-9]+' | head -1)
if [[ $(printf '%s\n0.20\n' "$ver" | sort -V | head -1) != 0.20 ]]; then
  warn "labwc $ver is older than 0.20: thumbnails in Alt+Tab, the magnifier and some keybinds won't work"
fi

# ------------------------------------------------------------------ backup + install helpers
# First install only: keep the user's original file, or note that it didn't exist (uninstall.sh deletes those).
backup() {
  [[ -e $bak/files$1 || -L $bak/files$1 ]] && return
  grep -qxF "$1" "$bak/created" 2>/dev/null && return
  if [[ -e $1 || -L $1 ]]; then
    mkdir -p "$bak/files$(dirname "$1")"
    cp -a "$1" "$bak/files$1"
  else
    mkdir -p "$bak"
    echo "$1" >>"$bak/created"
  fi
}
# Re-runs replace the configs with the repo's versions again. If yours differs (your own edits, or an older
# version), it is kept next to it as <file>.before-update-<time> first.
rerun=; [[ -d $bak ]] && rerun=1
stamp=$(date +%Y%m%d-%H%M%S)
place() {
  backup "$2"
  if [[ $rerun && -f $2 ]] && ! cmp -s "$1" "$2"; then
    cp -a "$2" "$2.before-update-$stamp"
    warn "kept your previous $(basename "$2") as $2.before-update-$stamp"
  fi
  install -Dm"${3:-644}" "$1" "$2"
}

# lxqt-panel writes panel.conf back when it exits, so it must be stopped while the file is replaced
panel_running() { pgrep -u "$USER" -x lxqt-panel >/dev/null; }
panel() {
  busctl --user call --expect-reply=false org.lxqt.session /LXQtSession org.lxqt.session "$1Module" s \
    lxqt-panel.desktop 2>/dev/null || true
}

tmp=$(mktemp -d)
restart_panel=
trap 'rm -rf "$tmp"; [[ -z $restart_panel ]] || panel start' EXIT  # never leave the panel stopped

# ------------------------------------------------------------------ scripts + native helpers
say "scripts -> $bin"
for f in "$here"/bin/*; do
  if [[ -f $f ]]; then place "$f" "$bin/$(basename "$f")" 755; fi
done
cc -O2 -Wall -o "$tmp/panelstat" "$here/src/panelstat/panelstat.c"
place "$tmp/panelstat" "$bin/panelstat" 755

say "building quickpanel (tray popups)"
cmake -S "$here/src/quickpanel" -B "$state/build-quickpanel" -DCMAKE_INSTALL_PREFIX="$HOME/.local" >/dev/null
cmake --build "$state/build-quickpanel" -j"$(nproc)"
backup "$bin/quickpanel"; backup "$bin/wifi-panel"
cmake --install "$state/build-quickpanel" >/dev/null

# ------------------------------------------------------------------ configs
say "configs -> $cfg"
themer=; [[ -x $bin/themer ]] && themer=1
ulp=; [[ -x /usr/local/bin/ultra-low-power ]] && ulp=1

place "$here/config/labwc/rc.xml" "$cfg/labwc/rc.xml"
place "$here/config/labwc/autostart" "$cfg/labwc/autostart"
place "$here/config/swayosd/config.toml" "$cfg/swayosd/config.toml"

# menu + panel: drop the plasma-themer / ultra-low-power entries when those aren't installed
cp "$here/config/labwc/menu.xml" "$tmp/menu.xml"
[[ $themer ]] || sed -i '/plasma-themer\|themer bg/d' "$tmp/menu.xml"
[[ $ulp ]] || sed -i '/ultra-low-power/d' "$tmp/menu.xml"
place "$tmp/menu.xml" "$cfg/labwc/menu.xml"

sed "s|@HOME@|$HOME|g" "$here/config/lxqt/panel.conf" >"$tmp/panel.conf"
[[ $themer ]] || sed -i '/^icon=.*plasma-themer/d; /^ownIcon=true/d' "$tmp/panel.conf"
[[ $ulp ]] || sed -i 's/, eco,/,/; /^\[eco\]/,/^$/d' "$tmp/panel.conf"
if panel_running; then panel stop; sleep 1; restart_panel=1; fi
place "$tmp/panel.conf" "$cfg/lxqt/panel.conf"

# LXQt: labwc as the compositor, no second battery icon (quickpanel has one)
# Only back these up if they exist: LXQt writes its own settings into them later, so uninstall mustn't delete them
for f in "$cfg/lxqt/session.conf" "$cfg/lxqt/lxqt-powermanagement.conf"; do
  if [[ -e $f ]]; then backup "$f"; fi
done
python3 - "$cfg/lxqt" <<'EOF'
import configparser, io, sys
from pathlib import Path
d = Path(sys.argv[1])
for name, sec, key, val in (("session.conf", "General", "compositor", "labwc"),
                            ("lxqt-powermanagement.conf", "General", "showIcon", "false")):
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    cp.optionxform = str
    cp.read(d / name)
    if not cp.has_section(sec):
        cp.add_section(sec)
    cp[sec][key] = val
    buf = io.StringIO()
    cp.write(buf, space_around_delimiters=False)
    d.mkdir(parents=True, exist_ok=True)
    (d / name).write_text(buf.getvalue())
EOF

# Turn off LXQt's desktop (swaybg draws the wallpaper) and the always-running runner (Alt+F2 starts it on demand)
for a in lxqt-desktop lxqt-runner; do
  printf '[Desktop Entry]\nType=Application\nName=%s\nHidden=true\n' "$a" >"$tmp/$a.desktop"
  place "$tmp/$a.desktop" "$cfg/autostart/$a.desktop"
done
sed "s|@BIN@|$bin|" "$here/config/save-monitor-layout.desktop" >"$tmp/save-monitor-layout.desktop"
place "$tmp/save-monitor-layout.desktop" "$HOME/.local/share/applications/save-monitor-layout.desktop"

# ------------------------------------------------------------------ touchpad gestures need the input group
relogin=
if ! id -nG "$USER" | grep -qw input; then
  say "adding $USER to the input group (touchpad gestures read libinput events)"
  sudo usermod -aG input "$USER" && relogin=1
fi

if [[ $patch ]]; then
  if [[ $family == arch ]]; then
    say "building labwc with the Alt+Tab close button"
    "$here/patches/labwc/rebuild.sh"
  else
    warn "--labwc-patch is Arch-only; see patches/labwc/ to apply switcher-close-button.patch yourself"
  fi
fi

# ------------------------------------------------------------------ apply now if we're inside labwc
if [[ $restart_panel ]]; then panel start; restart_panel=; fi
if pgrep -u "$USER" -x labwc >/dev/null; then
  labwc --reconfigure 2>/dev/null || true
  if pgrep -u "$USER" -x quickpanel >/dev/null; then
    pkill -u "$USER" -x quickpanel
    # its tray icons only register once the panel's tray is up again
    for _ in $(seq 30); do busctl --user status org.kde.StatusNotifierWatcher >/dev/null 2>&1 && break; sleep 0.5; done
    setsid -f "$bin/quickpanel" --daemon >/dev/null 2>&1
  fi
fi

echo
say "done"
echo "Log out and pick \"LXQt (Wayland)\" on the login screen (labwc is set as its compositor)."
[[ $relogin ]] && echo "Log out once anyway: joining the input group only takes effect at the next login."
echo "Undo everything with: $here/uninstall.sh"
