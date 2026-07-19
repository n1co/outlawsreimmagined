# Outlaws Engine - Required Game Files

This engine requires the original Outlaws (1997) game data files.
Place the following files in a `data/` directory next to the executable.

## Required LAB Archives

| File         | Description                        |
|--------------|------------------------------------|
| outlaws.lab  | Main game data (menus, fonts, UI)  |
| OLGEO.LAB    | Level geometry (LVT/OBT/INF files) |
| oltex.lab    | Level textures (PCX, ATX)          |
| olsfx.lab    | Sound effects (WAV)                |
| olweap.lab   | Weapons (ITM, NWX, WAV)            |
| olobj.lab    | Objects (3DO, NWX, ITM)            |

## Optional LAB Archives

| File         | Description                        |
|--------------|------------------------------------|
| oltaunt.lab  | Character voice taunts             |
| olpatch1.lab | Patch 1 content                    |
| olpatch2.lab | Patch 2 content                    |
| olpatch3.lab | Patch 3 content                    |

## Music

| File              | Description          |
|-------------------|----------------------|
| MUSIC/Track02.ogg | Level music tracks   |
| MUSIC/Track03.ogg | (Tracks 02-16)       |
| ...               |                      |

## File Formats

All formats are from the LucasArts Jedi Engine (1995-1997).

| Extension | Format Description                              |
|-----------|-------------------------------------------------|
| .lab      | LAB binary archive (LABN header)                |
| .lvt      | Level geometry (text, sectors/walls/vertices)   |
| .lvb      | Level geometry binary (precompiled cache)       |
| .obt      | Object table (text, entity placement)           |
| .obb      | Object binary (precompiled cache)               |
| .inf      | Interactive scripts (text, elevators/triggers)  |
| .pcx      | Texture/sprite image (ZSoft PCX, 256-color)     |
| .atx      | Animated texture (text, references PCX frames)  |
| .nwx      | Sprite/WAX animation (binary, WAXF format)      |
| .3do      | 3D model (text, vertices/triangles)             |
| .itm      | Item/entity definition (text)                   |
| .msc      | Music cue script (text)                         |
| .wav      | Sound effect (standard RIFF/WAV)               |
| .phy      | Physics material (text)                         |

## Directory Layout

```
data/
├── outlaws.lab
├── OLGEO.LAB
├── oltex.lab
├── olsfx.lab
├── olweap.lab
├── olobj.lab
└── MUSIC/
    ├── Track02.ogg
    ├── Track03.ogg
    └── ...
```
