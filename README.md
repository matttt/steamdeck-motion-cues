# Motion Cue Dots

A small Decky plugin that draws accelerometer-driven cue dots over games. This is the MVP extracted from OverLaid: there is no overlay manager, no widget model, and no image/text overlay customization.

## Controls

- Enable: toggles the cue overlay.
- Simulate motion: uses a looping car-like motion pattern instead of sensor input.
- Dot color: hex color field plus presets.
- Dot amount: controls dot density.
- Transparency: controls dot alpha.
- Motion response: scales how strongly acceleration affects flow and pulsing.

## Build

Install frontend dependencies, then build the Decky bundle:

```sh
pnpm install
pnpm run build
```

Build the native renderer on a Steam Deck or compatible Linux environment with GLFW, OpenGL, X11, and hidraw headers:

```sh
pnpm run build:backend
```

The native executable is copied to both `backend/out/SteamDeckMotionCues` and `bin/SteamDeckMotionCues`, which is the runtime path used by the plugin.
