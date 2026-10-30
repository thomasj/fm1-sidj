# SiDJ over MIDI

An addendum to the [user manual](../README.md#user-manual): playing the remix from a MIDI
keyboard or controller instead of the panel. Everything here works over USB or the MIDI IN
jack, on the player. SiDJ sends no MIDI back.

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
