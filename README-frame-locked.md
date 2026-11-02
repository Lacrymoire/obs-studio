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
| Game Capture | Buffering | on | Frames are chosen on the game's timeline; off = stock (newest frame). Changing it re-hooks the game. |
| Game Capture | Buffer delay (frames) | 2 | Video delay; the properties show the matching audio Sync Offset. |
| Game Capture | Frame ring limit | 32 | Most frames the hook may keep; the ring starts small and grows only when the game needs it (each slot is one frame of video memory, 33 MB at 4K). 1 = stock single texture. |
| Game Capture | Frame-lock debug log | off | Writes `<recording>.frames.csv` next to each recording, for diagnosis. |
| `config\obs-studio\basic\profiles\<profile>\basic.ini`, section `[Video]` | `GameClockOutput` | true | OBS's ticks follow the game's clock while a buffering Game Capture source is capturing. |
| same | `GameClockRetime` | true | With the above: constant frame rate file at the game's rate (true) or variable frame rate file with real-time timestamps (false). |

To record exactly as stock OBS does, close OBS and set `GameClockOutput=false`
in `[Video]`, or turn Buffering off on the source. Both game-clock settings
apply to every output, streaming included; streaming and the replay buffer have
not been tested with them.

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
- Vulkan and OpenGL games load the hook from
  `%ProgramData%\obs-studio-hook`. This build keeps that copy equal to its own
  hook at start; a stock OBS started later replaces it with the stock hook.
- Below about 57 fps on a 60 fps canvas (more than 5 % slow) no cadence fits
  and the source records like the plain buffered mode.

## Building

Build like OBS Studio 33.0.1 (Visual Studio, CMake preset `windows-x64`). The
changes are in `plugins/win-capture` (hook and Game Capture source),
`shared/obs-hook-config`, a few files in `libobs` (video pacing, encoder
timestamps, audio follower), the NVENC and x264 encoders, both MP4 muxers, and
`frontend/widgets/OBSBasic.cpp` (the two settings above).
