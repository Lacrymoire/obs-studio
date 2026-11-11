# OBS Studio, frame-locked build

OBS Studio 33.0.1 with a frame-locked Game Capture, for recording games without
stutter. Windows only (64-bit OBS, 32- and 64-bit games).

Source: https://github.com/Lacrymoire/obs-studio, branch `frame-locked`.
OBS Studio and this build are licensed under the GNU GPL version 2 or later
(see COPYING in the source). The exact commit is in the zip name and the OBS
log (`git describe`).

## What it changes

Stock OBS shows whatever frame the game presented last at each of its own ticks.
The game's clock and OBS's clock are never exactly equal, so every few seconds
the capture repeats one frame and skips the next, in bursts that last 1-6 s.

This build gives Game Capture the buffering a capture card has. The hook inside
the game keeps a short ring of frames, each stamped with the time the game
presented it, and OBS shows the frame that belongs to each tick on a smoothed
game timeline, slightly delayed. On top of that:

- **Game clock (on by default).** OBS's video ticks follow the game's clock, so
  every game frame is recorded exactly once. The file stays a normal constant
  frame rate file (60 fps on a 60 fps canvas) that runs at the game's rate: a
  game at 59.8 fps gives a file 0.3 % faster than real time. The audio is
  trimmed by the same amount and stays in sync.
- **Every frame rate.** 30, 40, 60, 120, 144 fps and capped rates in between
  get a fixed cadence (for example every second frame of a 120 fps game on a
  60 fps canvas). Uncapped games fall back to the plain buffered mode.
- **Frame generation.** DLSS multi-frame generation bursts are spread over the
  render interval and recorded in order.
- **Every graphics API.** Direct3D 9, 10, 11, 12, OpenGL and Vulkan.
- **Overlays.** With "Capture third-party overlays" off, the RivaTuner (RTSS)
  on-screen display stays out of the video.

## How to use

1. Unzip anywhere, not over a normal OBS install: the hook DLLs and OBS must
   come from the same build. Run `bin\64bit\obs64.exe`. It runs in portable
   mode: settings, scenes and logs live in the `config\` folder next to `bin\`.
2. Add one Game Capture source for the game. Buffering is on by default with a
   delay of 2 frames.
3. The buffer delays the video by delay x frame time (33 ms at 2 frames and
   60 fps). Set the same Sync Offset (+33 ms) on your audio sources in Audio
   Mixer > Advanced Audio Properties. The source's properties show the exact
   number for your frame rate and delay.
4. Record. For the smoothest result lock the game's frame rate (in-game limiter
   or RTSS) at or below the canvas rate, or at a whole multiple of it.

## Settings

| Where | Setting | Default | Effect |
|---|---|---|---|
| Game Capture | Buffering | on | Frames are chosen on the game's timeline; off = stock (newest frame). Takes effect at once (the capture restarts once). |
| Game Capture | Buffer delay (frames) | 2 | Video delay; the properties show the matching audio Sync Offset. |
| Game Capture | Frame ring limit | 32 | Most frames the hook may keep; the ring starts small and grows only when the game needs it (each slot is one frame of video memory, 33 MB at 4K). 1 = stock single texture. |
| Game Capture | Frame-lock debug log | off | Writes `<recording>.frames.csv` next to each recording, for diagnosis. |
| `config\obs-studio\basic\profiles\<profile>\basic.ini`, section `[Video]` | `GameClockOutput` | true | OBS's ticks follow the game's clock while a buffering Game Capture source is capturing. |
| same | `GameClockRetime` | true | With the above: constant frame rate file at the game's rate (true) or variable frame rate file with real-time timestamps (false). |

To record exactly as stock OBS does, close OBS and set `GameClockOutput=false`
in `[Video]`, or turn Buffering off on the source. Both game-clock settings
apply to every output. **Set `GameClockOutput=false` before streaming**: the
retimed file counts frames, not seconds, so a live stream would run short of
data or build up delay whenever the game is not at exactly the canvas rate.
The replay buffer has not been tested with them.

## Good to know

- Use one Game Capture source per game. Two sources capturing the same game
  share the hook's ring and the game clock.
- With a ring in use every present is copied: "Limit capture framerate" has no
  effect, and a menu running at 1000 fps means 1000 GPU copies per second.
- If the game runs out of video or system memory for the ring, the hook keeps
  a smaller ring until the game exits; the OBS log says "its ring is limited".
- An alt-tab out of a fullscreen DirectX game restarts the capture once; the
  recording shows the last frame for that moment.
- Overlays: the check runs when the capture starts, so RTSS started in the
  middle of a recording stays in the video until the next capture start. In
  games with DLSS frame generation (Streamline) keep RTSS at its default
  settings; with "Custom Direct3D support" on, its display can end up in the
  video, and Witcher 3 crashed at start.
- The hook in `%ProgramData%\obs-studio-hook` is the one Vulkan and OpenGL
  games load at start, and the only one Microsoft Store and Game Pass games can
  load. This build makes that copy equal to its own hook when OBS starts and
  injects it while it stays so. A stock OBS started later replaces it with the
  stock hook: Vulkan/OpenGL games then load the stock hook, other games get
  this build's own copy, and Store games cannot be captured until this build
  is restarted.
- A game slower than the canvas rate gets a pattern of planned repeats that
  matches its rate (50 fps: 6 ticks per 5 frames, 55 fps: 12 per 11), and the
  file keeps real-time speed. A game faster than the canvas rate that is not at
  a whole multiple of it (116, 138 fps) gets the nearest short pattern, and the
  file runs up to 3 % fast or slow, pitch included: cap such games at the
  canvas rate or a whole multiple.
- Shader mods: with "Capture third-party overlays" off, this build captures
  the frame before anything that hooks the present call. ReShade installed as
  a Vulkan layer, and ReShade or Special K injected globally, are then visible
  on screen but missing from the video. Tick "Capture third-party overlays"
  for those games (and hide the RTSS display yourself). Mods installed as a
  DLL in the game folder (`dxgi.dll`, `d3d9.dll`) are recorded either way.
- HDR: a game with a scRGB (16-bit float) swap chain is tone-mapped to the SDR
  canvas automatically. A game with an HDR10 (10-bit PQ) swap chain looks
  washed out unless the source's "RGB10A2 Color Space" is set to
  "Rec. 2100 (PQ)", as in stock OBS.
- Microsoft Store and Game Pass games: start this build once as administrator
  so it can give `%ProgramData%\obs-studio-hook` the permission those games
  need (stock OBS's installer does the same).
- Updates: the package contains an empty `disable_updater.txt` next to
  `portable_mode.txt`. Without it OBS offers the next stock release at start,
  and "Update Now" would replace this build with stock OBS.
- "Capture any fullscreen application" hooks whatever is fullscreen in front
  when no game is captured, including a video player or a browser that is not
  on OBS's skip list. Close them before a session or pick the game's window.

## Building

Build like OBS Studio 33.0.1 (Visual Studio, CMake preset `windows-x64`). The
changes are in `plugins/win-capture` (hook and Game Capture source),
`shared/obs-hook-config`, a few files in `libobs` (video pacing, encoder
timestamps, audio follower), the NVENC and x264 encoders, both MP4 muxers, and
`frontend/widgets/OBSBasic.cpp` (the two settings above).
