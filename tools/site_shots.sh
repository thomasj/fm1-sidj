#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The site's screenshots (web/site/img), rendered by the simulator from HVSC tunes
# ($HVSC, default ~/Music/C64Music) and tubesockor's two in web/site/songs:
#   songs.png      the song list: the FM-1's own songs (a run of their own, songs.in), tubesockor's
#                  two among them, Debut & Credit selected
#   remix.png      Commando at 1:09, voice 1 an octave up (KNOB 2): a square, noise and a pulse
#                  (the hero)
#   both.png       Wizball, out to the emulated SID and ASID at once
#   transpose.png  The Last Ninja, the keys transposing it five semitones down (the status line
#                  says so)
#   remix-4x.png   the hero, 4x with square pixels: the page rotates it, and a rotated 240-pixel
#                  image enlarged by the browser looks rough
# The list is grouped by composer (each group in the order added) and keeps the selected song in
# the middle, snapped to a row: Last Ninja, Cybernoid II, Wizball, Parallax, Commando,
# Streetlights and Dreams, Debut & Credit, TwinCross, Bionic Commando, Ghouls'n Ghosts, Black
# Lamp. One song each for Hubbard and Proton puts the Hubbard heading at the top with Debut &
# Credit selected. From Commando, Debut & Credit is 2 down, Wizball 2 up, The Last Ninja 2 more.
# Run it after a change to the screens; host/build_host.sh first.
set -e
cd "$(dirname "$0")/.."
H="${HVSC:-$HOME/Music/C64Music}"
out=build/site_shots
mkdir -p "$out"
# the tunes, with their HVSC song lengths (as tools/fm1_sid.py put gives them)
.venv/bin/python3 - "$H" > "$out/site.in" <<'PY'
import sys
sys.path.insert(0, "tools")
import fm1_sid
h = sys.argv[1]
sl = fm1_sid.Songlengths(h)
for f in [h + "/MUSICIANS/H/Hubbard_Rob/Commando.sid", "web/site/songs/Debut_and_Credit.sid",
          "web/site/songs/TwinCross.sid", h + "/MUSICIANS/G/Galway_Martin/Wizball.sid",
          h + "/MUSICIANS/T/Tel_Jeroen/Cybernoid_II.sid", h + "/MUSICIANS/P/Proton/Streetlights_and_Dreams.sid",
          h + "/MUSICIANS/D/Daglish_Ben/Last_Ninja.sid", h + "/MUSICIANS/G/Galway_Martin/Parallax.sid",
          h + "/MUSICIANS/F/Follin_Tim/Bionic_Commando.sid", h + "/MUSICIANS/F/Follin_Tim/Ghouls_n_Ghosts.sid",
          h + "/MUSICIANS/F/Follin_Tim/Black_Lamp.sid"]:
    print("sid", f, *sl.lookup(open(f, "rb").read())[1])
PY
cat >> "$out/site.in" <<SCN
turn SELECT 2
wait 2500
shot songs.png
turn SELECT -2
tap PLAY
wait ${HERO_MS:-68200}
turn K2 1
wait ${HERO_WAIT:-1150}
shot remix.png
turn SELECT -2
turn ALGO 2
wait ${BOTH_MS:-22000}
shot both.png
turn ALGO 1
turn SELECT -2
wait ${NINJA_MS:-56400}
press FX
tap OCT-
release FX
key 8 down
wait 600
shot transpose.png
key 8 up
SCN
build/host/sidj_host "$out/site.in" "$out" > "$out/site.log"
# the song list: the songs on the FM-1, in the order added
.venv/bin/python3 - "$H" > "$out/songs.in" <<'PY'
import sys
sys.path.insert(0, "tools")
import fm1_sid
h = sys.argv[1]
sl = fm1_sid.Songlengths(h)
m = h + "/MUSICIANS/"
for f in [m + "H/Hubbard_Rob/Commando.sid", "web/site/songs/Debut_and_Credit.sid", "web/site/songs/TwinCross.sid",
          m + "G/Galway_Martin/Wizball.sid", m + "T/Tel_Jeroen/Cybernoid_II.sid", m + "D/Daglish_Ben/Last_Ninja.sid",
          m + "G/Galway_Martin/Parallax.sid", m + "F/Follin_Tim/Black_Lamp.sid", m + "S/Shogoon/Circles.sid",
          m + "S/Shogoon/Kickin_Balls.sid", m + "S/Shogoon/Micro_World.sid", m + "D/Danko_Tomas/Dankoverse.sid",
          m + "M/Mibri/Dabke_8580.sid", m + "P/Psych858o/Like_We_Never_Left.sid", m + "V/Vincenzo/Blitzer_Marathon.sid",
          m + "S/Steel/Beautiful_Struggle.sid", m + "L/Linus/Auf_Wiedersehen_Linus.sid", m + "C/C0zmo/Rimini_Remedy.sid",
          m + "T/The_Syndrom/Wanted_or_Needed.sid", m + "D/Danko_Tomas/Wonderland_X_Introtune.sid",
          m + "S/Swallow/Wonderland_X_part_1.sid", m + "L/LFT/A_Mind_Is_Born.sid", m + "L/LFT/Hardsync.sid",
          m + "L/LFT/Elvendance.sid", m + "S/Shogoon/Ballad_for_Jeff.sid", m + "T/Tel_Jeroen/Supremacy.sid"]:
    print("sid", f, *sl.lookup(open(f, "rb").read())[1])
PY
cat >> "$out/songs.in" <<SCN
wait 4500
turn SELECT ${SONGS_SEL:-2}
wait 2500
shot songs.png
SCN
build/host/sidj_host "$out/songs.in" "$out" > "$out/songs.log"
.venv/bin/python3 - "$out" <<'PY'
import sys
from PIL import Image
src = sys.argv[1]
for f in ("songs", "remix", "both", "transpose"):        # the simulator's PNGs, compressed
    Image.open(f"{src}/{f}.png").save(f"web/site/img/{f}.png", optimize=True)
Image.open(f"{src}/remix.png").resize((960, 960), Image.NEAREST).save("web/site/img/remix-4x.png", optimize=True)
PY
echo "web/site/img: songs.png remix.png both.png transpose.png remix-4x.png"
