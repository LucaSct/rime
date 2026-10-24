# Audio attribution

Every file here is a recording released under **CC0 1.0 (public domain dedication)**. CC0 asks for
no attribution; it is given anyway. Licences were confirmed on each source page on 2026-10-21 (the
Freesound pages show "Creative Commons 0"; the firearm library states "CC0 NO RIGHTS RESERVED").
No Battlefield / game rips and no "royalty-free, not redistributable" packs (e.g. Sonniss) are used.

Processing applied to all files (script: none committed; steps are exactly these): decode with
ffmpeg, mix to mono, resample to 48 kHz, cut the named segment (aligned a few ms before the first
transient), apply a 2 ms fade-in and a cosine fade-out, remove DC/rumble below 25 Hz with a 2nd-order
Butterworth high-pass, normalise to a 0.9 peak, store as 16-bit PCM WAV. The `rumble_*` files are
additionally low-passed (4th-order Butterworth) at the frequency given below with a 0.25 s fade-in.
Freesound files are the site's 128 kbit/s MP3 previews (the originals need a login), so they carry
MP3 coding loss; the firearm files are decoded from the library's 96 kHz / 24-bit WAVs.

## Weapons: The Free Firearm Sound Library (Prepared SFX Library)

- Source: https://opengameart.org/content/the-free-firearm-sound-library
  (project site http://freefirearmsfx.com)
- Authors: Ben Jaszczak, Brian Nelson, Kevin Heras, Matthew Nanney. Licence: CC0.
- Outdoor "near distance" takes; each take holds several shots, one shot cut per file.

| file | library file | what it is | segment(s) |
|---|---|---|---|
| gun_scar_0..2.wav | AK-47/C_28P.wav | AK-47, 7.62x39, near | shots 1-3 (0.61 s, 3.25 s, 6.02 s) |
| gun_m4_0..1.wav | AR-15/D_32P.wav | AR-15 / M4, 5.56x45, near | shots 1-2 (0.70 s, 5.65 s) |
| gun_m4_2.wav | AR-15/D_24P.wav | AR-15 / M4, mid distance | shot 1 (0.55 s) |
| gun_sniper_0..2.wav | Mosin Nagant/M_21P.wav | Mosin Nagant, 7.62x54R, near | shots 1-3 (1.03 s, 5.01 s, 9.15 s) |

The game has no sidearm: its three weapons are the SCAR-H (served by the AK-47 recordings, the
closest full-power 7.62 rifle in a CC0 set), the M4A1 (AR-15 recordings) and the MSR sniper (Mosin).

## Masonry impacts, breaks, rubble (Freesound, all CC0)

| file | Freesound sound | author | segment |
|---|---|---|---|
| impact_0.wav | https://freesound.org/people/InMotionAudio/sounds/702860/ "ROCKImpt_Brick25" | InMotionAudio | 0-0.45 s |
| impact_1.wav | https://freesound.org/people/Hankof/sounds/655622/ "brick falling on brick" | Hankof | 0-0.5 s |
| impact_2.wav | https://freesound.org/people/Sheyvan/sounds/569510/ "Stone Impact Rubble Debris 14" | Sheyvan | 0-0.5 s |
| impact_3.wav | https://freesound.org/people/jadevelloza/sounds/655350/ "Brick being dropped OWI" | jadevelloza | impact at ~0.5 s, 0.55 s long |
| break_0..4.wav | https://freesound.org/people/Nox_Sound/sounds/567701/ "Foley_Rocks_Stones_Impacts_Mono" | Nox_Sound | five separate impacts (4.11, 12.4, 19.16, 27.45, 1.56 s), 0.75 s each |
| settle_0..3.wav | https://freesound.org/people/Kastenfrosch/sounds/521956/ "Stone Crumble 2" | Kastenfrosch | four 0.45 s windows from the first 2.2 s |
| settle_4..5.wav | https://freesound.org/people/Briarheart02/sounds/462162/ "Rubble shifting" | Briarheart02 | 1.0-1.6 s and 1.7-2.3 s |
| collapse_0.wav | https://freesound.org/people/xkeril/sounds/703247/ "Big falling debris (crash)" | xkeril | 0-3.0 s |
| collapse_1.wav | https://freesound.org/people/xkeril/sounds/703248/ "Fall debris (crash)" | xkeril | 0-2.6 s |
| collapse_2.wav | https://freesound.org/people/craigsmith/sounds/675899/ "S30-08 Wall falls down from fire damage" | craigsmith | 2.7-5.5 s |
| collapse_3.wav | https://freesound.org/people/iwanPlays/sounds/567249/ "Bricks/Stones/Rocks/Gravel Falling" | iwanPlays | 0-3.0 s |
| rumble_0.wav | craigsmith 675899 (above) | craigsmith | 3.0-6.2 s, low-passed at 170 Hz |
| rumble_1.wav | xkeril 703247 (above) | xkeril | 0-3.0 s, low-passed at 150 Hz |
