


# Motion Cue Dots
https://github.com/user-attachments/assets/168230e9-617b-44dc-973d-01c738e015c8

A small Decky plugin that draws accelerometer-driven cue dots over games. 

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


## Attribution
Huge shoutout to TheLogicMaster on github and his [Overlaid repo](https://github.com/TheLogicMaster/OverLaid), from which I learned how to build an overlay and integrate it into a Decky plugin. 

