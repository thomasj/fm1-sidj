# SiDJ over MIDI

An addendum to the [user manual](../README.md#user-manual): playing the remix from a MIDI
keyboard or controller instead of the panel, over USB or the MIDI IN jack, on the player; the MIDI
clock SiDJ sends in time with a tune; and a clock coming in, which the tune follows.

## Channels

Channels 1, 2 and 3 address voices 1, 2 and 3. Any other channel addresses the voice selected
on the panel. The global CCs take any channel.

## Notes

Notes play like the panel's keys, in the same mode: in *set* mode (the default) the voice holds
the note while the key is down; in *transpose* mode the tune's melody moves by the note's
distance from F#4.

## CCs

The whole remix has a CC each. Where the tune has a value of its own, an end or a zone of the
range gives it back. Whatever a CC changes turns gold on screen, as from the panel, and CC 108
puts everything back.

| CC | Per voice | 0 to 127 |
| --- | --- | --- |
| 70 | pulse width | 0 the tune's, then 0 to 4095 |
| 73 | attack | 0 the tune's, then 0 to 15 |
| 75 | decay | 0 the tune's, then 0 to 15 |
| 76 | sustain | 0 the tune's, then 0 to 15 |
| 72 | release | 0 the tune's, then 0 to 15 |
| 77 | octave | seven zones, -3 to +3; 64 is 0 |
| 78 | fine tune | 64 is 0, the ends ±50 cents |
| 79 | waveform | five zones: the tune's, pulse, triangle, sawtooth, noise |
| 80 | sync | three zones: the tune's, off, on |
| 81 | ring modulation | three zones: the tune's, off, on |
| 82 | mute | 64 and up muted |
| 83 | solo | 64 and up the voice alone, below all three |
| 84 | through the filter | three zones: the tune's, around, through |
| 85 | LFO rate | slow to fast |
| 86 | LFO depth | 0 to 100 % |
| 87 | LFO target | four zones: off, pulse width, pitch, cutoff |
| 88 | LFO shape | five zones: sine, triangle, saw, square, random |

| CC | Global | 0 to 127 |
| --- | --- | --- |
| 74 | cutoff | scaling: 64 the tune's, twice or half per 16 (127 about x16); adding: 64 is 0, the ends ±2047 |
| 71 | resonance | 64 the tune's, the ends ±15 |
| 102 | cutoff mode | below 64 scaling, 64 and up adding |
| 103 | low-pass | three zones: the tune's, off, on |
| 104 | band-pass | three zones: the tune's, off, on |
| 105 | high-pass | three zones: the tune's, off, on |
| 106 | tempo | 0 is 25 %, 64 100 %, 127 150 % |
| 107 | vinyl fader | 64 is 0, the ends ±8 % |
| 108 | back to the tune | 64 and up: everything back, as `OCT-` and `OCT+` together |

71 to 75 are the standard sound controllers (resonance, release, attack, cutoff, decay). 91 to
95, which many keyboards send for reverb and chorus, are left alone.

RSID tunes play as written, so CCs 85 to 88, 106 and 107 (the LFOs, the tempo and the fader) do
nothing there.

## MIDI clock

With the output on SID+CLK (`KNOB 2` in SONGS), SiDJ sends a MIDI clock over USB in time with the
tune: MIDI Start on the tune's first row, then 24 pulses a beat. It follows the tempo and the vinyl
fader. A pause sends Stop, PLAY after it Continue, and a new song or subsong starts over with Stop
and Start. The FM-1 has no MIDI out jack, so the clock reaches other gear through a computer or a
USB-MIDI host box. The player shows the clock's BPM next to the play symbol.

**Finding the beat.** The beat comes from the tune itself. Trackers start notes only on rows,
every few player calls, and a row is nearly always a 16th note. When a song loads, SiDJ runs it
ahead silently (DETECTING TEMPO, one to three seconds), counts the player's calls by the bursts of
writes each one makes, and looks at when the voices start to sound:

- The grid is the longest row length that nearly all note starts land on, or two row lengths in
  turn when the tune swings (a long row then a short one, up to twice as long: a triplet shuffle).
  SiDJ takes the grid it still holds at the end of a twelve-second look, since an early guess can
  be a row half as long.
- The beat is four rows, or six when the tune goes in threes: its notes repeat more after 6, 12 or
  24 rows than after 4, 8, 16 or 32, and its odd rows are not nearly empty.
- The beat is halved or doubled to fall between 70 and 180 BPM.
- Beat 1 goes on the row of the beat that most notes land on, so a pickup does not shift it.

The tune then plays from its start, and the screen shows the tempo found, for example MIDI CLOCK:
125.3 BPM.

**Putting it right.** When the tempo found is wrong, mostly half or double, `KNOB 4` on the `GLO`
page puts the clock right from its next beat: half, two thirds, three quarters, as found, four
thirds, three halves or double. Double is also for a tune faster than 180 BPM, which SiDJ reads at
half. `KNOB 3` moves the clock earlier or later, 5 ms a step (25 turned fast), up to 250 ms either
way. Both change the clock only, not the tune: they are not part of the remix, and `OCT-` with
`OCT+` leaves them. The screen shows them in grey, K3 the offset and K4 the BPM.

**No tempo found.** There is no clock when no grid shows within the first twelve seconds: tunes in
free time, with sparse notes or sampled music, with a long quiet intro, and RSID tunes. Over HVSC
that is about a third of the tunes; the screen says NO TEMPO FOUND: SET ONE WITH GLO K4. Then
`KNOB 4` on the `GLO` page sets a tempo by hand. The first turn up starts the clock at about
120 BPM, at once. Each further step makes the beat one player call shorter or longer (three turned
fast): those are the tempos the tune can have, as it starts notes only on its calls. A tune called
once a frame on PAL steps 115.7, 120.3, 125.3; one called four times a frame, about 1.2 BPM.
REC & PLAY then puts beat 1 on the tune's start, and `KNOB 3` moves it. Turned down below 40 BPM,
the clock is off.

**Kept clocks.** A song keeps its clock for that song and subsong, by the file's MD5, so a setlist
loaded again keeps it too. That is the tempo found, the ratio and the offset once they are put
right, or the tempo set by hand. Its next load takes them without looking: MIDI CLOCK: 125.2 BPM,
KEPT. Turn `KNOB 4` or `KNOB 3` away and back to x1 and 0 ms, or a hand-set tempo below 40 BPM,
and the song forgets its clock: its next load looks again. REC & PLAY, or PLAY after the end, keeps
the tempo found as well.

**Hearing it.** `GLO` & `PLAY` adds a click on each of the clock's beats to the sound, to hear
where the clock is. The clicks are all alike, since SiDJ does not know the bar. Again turns it off,
and SAVE keeps it.

**Limits.** The clock follows the notes as the tune writes them: a tune whose notes swell in a
frame or two late (a silent first frame, a filter that opens after the note) has its clock a
little ahead of what you hear. A speed change later in a tune is not followed. Browsing with
`SELECT` loads songs without looking for their tempo; PLAY looks. Changing the output to SID+CLK
while a song plays starts the clock from the next load.

## MIDI clock in

SiDJ also takes a clock, over USB or the MIDI IN jack. With any output, MIDI Start plays the song
on screen from its start (in SONGS, the one selected), Stop pauses it and Continue goes on.

With the output on SID+CLK and a beat found, the tune follows the clock that comes in. Its speed is
set so its beats fall on the clock's, then nudged, at most 10 % either way, until they line up:
within a millisecond, about two seconds after a Start or a change of tempo. The tune's first beat
goes with the clock's nearest bar. While a clock comes in, SiDJ sends none of its own, the player
shows the clock's BPM, and the tempo knob waits until the clock is gone.

The clock's tempo is measured over its last bar, because USB lands each pulse up to a millisecond
late. A big change of tempo is taken within half a beat, a small one (1 BPM) within a second.

Half a second without a pulse is a clock gone, but a Stop is not: the song pauses with the clock's
tempo, and Continue or Start goes on from it. If the song is played from the panel instead and no
clock comes back, it goes back to its own tempo after a second.

`KNOB 4` on `GLO` still says what the tempo found is (half, double), and `KNOB 3` moves the tune
against the clock. A song that has not looked for its tempo yet looks first, so its first Start
comes a few seconds late; it then joins the clock on a bar.
