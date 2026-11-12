# Sample credits

The audio that ships with Neptune Groovebox, where it came from and under what terms. Every file in
`assets/breaks/` has a row here; a file without a row does not go into the repository.

## Rules for adding audio

- The recording itself must be freely licensed by the person who made it: CC0 / public domain, stated on the
  hosting page for that file or pack. CC-BY only with the exact attribution recorded here. Nothing
  "non-commercial", nothing "royalty-free" without a real licence, nothing whose origin cannot be shown.
- Nothing taken from a commercial record, and nothing derived from one, whatever licence a download page claims.
  That excludes the well-known classic breaks and every edit of them.
- Names in the app are descriptive (the tempo); credits live in this file.

## Drum breaks (`assets/breaks/`)

Six recordings of drummers playing their own kits, each released by the uploader on Freesound under
**CC0 1.0 Universal** (<https://creativecommons.org/publicdomain/zero/1.0/>). CC0 asks for no attribution; the
drummers are credited here all the same. All were fetched on 2026-11-11.

| File | In the app | Bars, tempo, length | Author (Freesound user) | Original title | Source page | Author's statement of origin (from the page) |
|---|---|---|---|---|---|---|
| `b080.flac` | B080 | 2 bars, 80 BPM, 6.000 s, mono | lennartgreen | "Fat Drum Loop 80 BpM 2" (WAV, 48 kHz, mono; pack "One Mic Mono Drums"), uploaded 2018-05-22 | <https://freesound.org/people/lennartgreen/sounds/429957/> | "Mapex Mars Big Rock recorded with one dynamic Sennheiser e835 over the kick facing the snare …" |
| `b090.flac` | B090 | 2 bars, 90 BPM, 5.332 s, stereo | Snapper4298 | "Tama_2_breaks.wav" (WAV, 44.1 kHz, stereo), uploaded 2013-09-19 | <https://freesound.org/people/Snapper4298/sounds/200422/> | "A breakbeat, live from my Tama Drum Kit, loops perfectly …" |
| `b105.flac` | B105 | 4 bars, 105 BPM, 9.143 s, stereo | lennartgreen | "105 BpM - Groove Syncopated - 02" (WAV, 48 kHz, stereo; pack "Loop Sessions 2026 Jan"), uploaded 2026-01-21 | <https://freesound.org/people/lennartgreen/sounds/842685/> | "I just had some fun coming up with a few ideas on my drum kit. I used a Mapex Armory kit … Mixed multitrack recording …" |
| `b120.flac` | B120 | 2 bars, 119.4 BPM, 4.020 s, stereo | dr_skitz | "drum_kit_acoustic120.wav" (WAV, 48 kHz, stereo), uploaded 2016-09-09 | <https://freesound.org/people/dr_skitz/sounds/353853/> | "Acoustic drum kit recording loop. 120 bpm approx. Recorded with 1 overhead and 1 kick drum microphone. Stereo-widened in the mix." |
| `b157.flac` | B157 | 1 bar, 156.5 BPM, 1.533 s, stereo | unfa | "D'n'B / breakbeat acoustic loop 157 BPM" (FLAC, 48 kHz, stereo; pack "Drums"), uploaded 2013-04-22 | <https://freesound.org/people/unfa/sounds/185615/> | "It's a breakbeat / drum and bass drumloop. No samples - I played this by myself. … Recorded with Zoom H2." |
| `b160.flac` | B160 | 4 bars, 160 BPM, 6.000 s, stereo | lennartgreen | "Uptempo Rock Drum Groove Upbeat 160 BpM" (WAV, 48 kHz, stereo; pack "Loop Sessions 2026 June"), uploaded 2026-06-28 | <https://freesound.org/people/lennartgreen/sounds/860527/> | "I just had some fun coming up with a few ideas on my drum kit. I used a Gretsch Renown drum kit … Mixed multitrack recording …" |

### What was fetched, and what was changed

Freesound gives the original file only to a logged-in account, so each break was made from the site's own
high-quality preview of that sound (Ogg Vorbis, about 192 kbit/s stereo or 110 kbit/s mono), which anyone can
fetch: `https://cdn.freesound.org/previews/<first digits>/<sound id>_<user id>-hq.ogg`. The preview is a lossy
copy of the same recording under the same licence. Someone with an account can replace a file with one made from
the original in the same way; lengths and names must stay the same.

| File | Fetched preview | Changes |
|---|---|---|
| `b080.flac` | `previews/429/429957_3661618-hq.ogg` | resampled 48 → 44.1 kHz; peak set to −1 dBFS |
| `b090.flac` | `previews/200/200422_33044-hq.ogg` | peak set to −1 dBFS |
| `b105.flac` | `previews/842/842685_3661618-hq.ogg` | resampled 48 → 44.1 kHz; peak set to −1 dBFS |
| `b120.flac` | `previews/353/353853_4161250-hq.ogg` | resampled 48 → 44.1 kHz; peak set to −1 dBFS |
| `b157.flac` | `previews/185/185615_1038806-hq.ogg` | resampled 48 → 44.1 kHz; peak set to −1 dBFS |
| `b160.flac` | `previews/860/860527_3661618-hq.ogg` | the first four of its eight bars (0 – 6.000 s) with a 12 ms fade-out at the cut; resampled 48 → 44.1 kHz; peak set to −1 dBFS |

All: decoded with SoX 14, written as 16-bit FLAC (lossless from there on), no other processing — no EQ,
compression, time correction or editing inside the loop. The command, per file:

```
sox -R <preview>.ogg -b 16 -r 44100 <name>.flac gain -3 [trim 0 6.0 fade t 0 6.0 0.012] rate -v gain -n -1
```

SHA-256 of the shipped files:

```
4c00c3e29eb7b9ad3f498c6892087051d3e9c01e37af38a163b3d41dcc03a05c  b080.flac
83355e71734111eae627167dee2136a179f1ed6efc9514dbcd8ea40b9400b012  b090.flac
5b8fece503674184ab2250072903e7baf498ddb34c134333f44daf454b7f07b6  b105.flac
0106dcc8b4a0ec40bd79f517b522fa230b1d72c83fd81df67f155a084c95b702  b120.flac
64b20f42b0a03d2ab72d4f118d45b5e638f9dcc493613c51b7d7382f7d64ebda  b157.flac
12a5638b104d403b08a23aac35040ee606de96f51fea48b0f09300278ed142e9  b160.flac
```

Total: 3.0 MB (3,048,367 bytes), 32.0 seconds of audio.

### How the licences were checked

For each sound the Freesound page was fetched on 2026-11-11 and read: the licence link on the page is CC0 1.0 and
no other Creative Commons licence appears on it, and the description (quoted above) says the uploader recorded an
acoustic kit. The lengths were then measured: each file is a whole number of bars at the stated tempo. Tempo and
length are measurements; that the uploader is the player, or had the player's agreement, rests on the uploader's
own statement, as it does for anything on Freesound.

Looked at and left out, as examples of the rule: every upload named after or cut from a classic break (even where
the page says CC0); loops whose description gives no origin; loops built from sample packs or drum machines'
factory sounds of unknown licence; a loop with "some electronic punchy kick added" of unknown origin; a recording
of a named session drummer uploaded by someone else.

## Breaks made by the app itself

B100 (2 bars at 100 BPM) and B170 (2 bars at 170 BPM) are not recordings: `SampleBank` renders them at start-up
from the app's own drum voices (`src/engine/SampleBank.h`, `buildBreaks`), as it does the hits, the BEAT loop and
the TONE. They are part of the program and carry its licence (AGPL-3.0); there is no file and nothing to credit.
