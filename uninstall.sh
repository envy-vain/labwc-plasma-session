#!/usr/bin/env bash
# Put back every file install.sh replaced and delete the ones it created. Packages stay installed.
set -euo pipefail
state=${XDG_STATE_HOME:-$HOME/.local/state}/labwc-plasma-session
bak=$state/backup
[[ -d $bak ]] || { echo "Nothing to undo: no backup in $bak" >&2; exit 1; }

# lxqt-panel writes panel.conf back when it exits, so stop it while the old one is restored
running=
if pgrep -u "$USER" -x lxqt-panel >/dev/null; then
  busctl --user call --expect-reply=false org.lxqt.session /LXQtSession org.lxqt.session stopModule s lxqt-panel.desktop 2>/dev/null || true
  sleep 1; running=1
fi
pkill -u "$USER" -x quickpanel 2>/dev/null || true

if [[ -f $bak/created ]]; then
  while IFS= read -r f; do rm -f -- "$f"; done <"$bak/created"
fi
if [[ -d $bak/files ]]; then
  (cd "$bak/files" && find . \( -type f -o -type l \) -print0) | while IFS= read -r -d '' f; do
    f=${f#.}
    mkdir -p "$(dirname "$f")"
    cp -a "$bak/files$f" "$f"
  done
fi
rm -rf "$state"

if [[ $running ]]; then
  busctl --user call --expect-reply=false org.lxqt.session /LXQtSession org.lxqt.session startModule s lxqt-panel.desktop 2>/dev/null || true
fi
if pgrep -u "$USER" -x labwc >/dev/null; then labwc --reconfigure 2>/dev/null || true; fi
echo "Restored your previous configs. Log out and back in to finish."
echo "Left as is: the packages, and your membership of the input group (leave it with: sudo gpasswd -d $USER input)."
echo "Patched labwc (if you built it): sudo pacman -S labwc"
