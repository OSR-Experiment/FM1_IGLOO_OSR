# Banquise

A small musical game about walking on an ice field. You start in front of your igloo; the wind, a drone and your footsteps make the music, and every place you find wakes the synth up in its own way: a fishing hole that bites on the beat, a stranded boat whose engine is the kick drum, an ice cave of tuned icicles, a forest of pines that ring when you brush them, a lighthouse that turns when you play, seals that sing you melodies to repeat.

There is a quest (the captain's journal, a lighthouse to relight, an ice flute, a lost drum machine), ten outfits, nine tapes for your radio, a dog called Seito, a sled driver called Yucca, and an old fisherman who only talks at night.

**Play it:** open `index.html` in a browser (English) or `fr.html` (Français). Turn the sound on, then walk.

## Controls

Banquise was designed around the controls of the M-VAVE FM-1 synth, and the on-screen buttons carry its names.

| FM-1 | Keyboard | What it does |
| --- | --- | --- |
| EDIT / PLAY / SEQ / REC | Arrow keys | Walk up / down / left / right |
| LFO | Space | Action (talk, fish, open, build a snowman…) |
| SCL | R | Change the chord grid |
| ENV | I | Switch instrument (synth, kalimba, ice flute) |
| HOME | L | Go home |
| ALGORITHM | — | Sky: day/night cycle, or a fixed sky |
| PRESET | — | Sound |
| K1–K4 | — | Wind, drone, steps, synth levels |
| Keys | A W S E D F T G Y H U J K | Play |

Your game is saved in your browser. "New game" at the top starts over.

## How it is made

Everything is in plain files, no build step:

- `index.html` / `fr.html`: the game (drawing, world, quest, sounds of the world)
- `glisse-dsp.js`: the synth voice, an AudioWorklet (subtractive, FM and plucked engines, rhythm sequencer, kick with sidechain)

It is part of the OSR-1 project, a custom firmware for the M-VAVE FM-1: [OSR-Experiment/OSR-1](https://github.com/OSR-Experiment/OSR-1).

## Credits

Made by Obsimo (OSR Records), with Claude. Fonts: Unbounded, Manrope and JetBrains Mono, served by Google Fonts (SIL Open Font License).

## License

GPL-3.0. See `LICENSE`.
