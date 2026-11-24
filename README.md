# SyphonLink

**Syphon server plugin for Unreal Engine 5.**

SyphonLink publishes any UE5 camera as a [Syphon](http://syphon.info) source — GPU-direct, no CPU readback — so your Unreal scenes appear live in Resolume, MadMapper, OBS, Max/Jitter, or any other Syphon-aware app on the same Mac.

Built for live audiovisual performance: pick a camera, name your server, hit Play.

![UE5 scene live in OBS via Syphon](Docs/scene-in-obs.png)

---

## Features

- **Syphon server component** — drop `SyphonServer` on any actor
- **Camera picker** — publish whichever camera you choose; switch cameras at runtime with the `Set Camera` Blueprint node (built for scene-switching VJ setups)
- **Two capture modes** (see [Capture modes](#capture-modes)):
  - **Main View** — publishes the camera's exact final image (lens, filmback, DOF, exposure, post-process, aspect ratio) at the game viewport's size, with no extra render cost
  - **Scene Capture** — renders the camera separately at a preset (720p / 1080p / 1440p / 4K) or custom resolution, independent of the window, optionally matching the camera's aspect ratio
- **Widget overlay** — draw UMG widgets into the Syphon image in either mode: widgets that are on screen, or widgets made only for the Syphon output
- **Opaque output** — frames are published with alpha = 1 by default, so clients that alpha-blend the source (Max/Jitter, compositors) don't show it as transparent or black
- **Metal-native** — uses Syphon's Metal API and IOSurface; frames stay on the GPU, with no CPU readback
- **Blueprint API** — Start Server, Stop Server, Set Camera, Set Capture Mode, Set Server Name; all properties editable live

## Requirements

- macOS on Apple Silicon (tested on M5)
- Unreal Engine 5.6 or later (tested on 5.6 and 5.7)
- Xcode installed (for building the plugin)

## Install

1. Download the latest release zip (or clone this repo)
2. Copy the `SyphonLink` folder into your project's `Plugins/` directory
3. If your project is Blueprint-only, add one empty C++ class first (Tools → New C++ Class → None) so the project can compile plugins
4. Build the editor target from Terminal:

```
"/Users/Shared/Epic Games/UE_5.6/Engine/Build/BatchFiles/Mac/Build.sh" \
  YourProjectEditor Mac Development \
  -project="/path/to/YourProject.uproject"
```

5. Open the project. SyphonLink appears under Edit → Plugins → Rendering.

## Quick start

1. Add an **Empty Actor** to your persistent level
2. **Add Component → Syphon Server**
3. Set **Server Name**, pick a **Camera Actor** and a **Capture Mode**:
   - **Main View**: the output size is the game viewport's size (set the PIE *New Window Size*, or enable **Apply Resolution To Window** for Standalone/packaged)
   - **Scene Capture**: choose a **Resolution** (or **Custom**)
4. Press **Play**
5. Open Simple Client (https://github.com/Syphon/Simple) or a tool of your choice


![Configuration in details view](Docs/component-details.png)

## Runtime camera switching (Blueprint)

For scene-based shows: after loading a new scene/sublevel, find its camera (e.g. by tag) and call **Set Camera** on the SyphonServer component. The output follows instantly.

```
Get All Actors With Tag ("SceneCam") → Get [0] → SyphonServer → Set Camera
```

## Capture modes

| Mode | What Syphon receives | Resolution | Cost |
|---|---|---|---|
| **Main View (exact camera)** — default | The game viewport's final image. The Camera Actor becomes player 0's view target, so *everything* the camera defines applies: CineCamera lens/filmback, depth of field, exposure, post-process settings and volumes, aspect-ratio constraint, camera shakes/fades. Captured after post-processing/upscaling and before UI, HUD and debug text. | Game viewport size (set the PIE *New Window Size*, or enable **Apply Resolution To Window** for Standalone/packaged) | None — no second render |
| **Scene Capture (independent)** | A separate Scene Capture that copies the camera's full view each frame (`GetCameraView`: location, rotation, FOV, projection, ortho width, post-process settings incl. CineCamera DOF). Leaves the player's view alone. | Any preset or **Custom**; **Match Camera Aspect** derives the height from the camera's aspect ratio | A second scene render |

Scene Capture is very close to the main view but not pixel-identical (temporal effects such as auto-exposure and TSR accumulate separately; motion blur is off).
Frames are published with one frame of latency so the GPU copy is always finished before Syphon reads it.

## Widgets (UMG) in the Syphon output

Both capture modes grab the image before Unreal draws its UI, so widgets added to the viewport don't reach Syphon on their own. Add them to the component instead and they are drawn on top of each frame, in the same Syphon source:

- **Overlay Widget Classes** (details panel): widget classes created at Begin Play and shown only in Syphon. Use **Get Overlay Widgets** to reach the instances.
- **Add Overlay Widget Instance** (Blueprint): send a widget you already created, for example your HUD after **Add to Viewport**. Syphon then shows the same instance as the screen, with the same state.
- **Add Overlay Widget** (Blueprint): create a widget from a class for Syphon only; returns the instance.
- **Remove Overlay Widget** / **Clear Overlay Widgets**, and **Render Widgets** to toggle the overlay live.

Widgets are drawn in list order (the first is at the bottom) and each fills the whole image, as with Add to Viewport. They use the UMG DPI scale for the output size, so in Main View the layout matches the screen; in Scene Capture it matches a screen of the capture's size. Hiding a widget (Visibility) hides it in Syphon too.

```
Create Widget (HUD) → Add to Viewport → SyphonServer → Add Overlay Widget Instance
```

## Component reference

| Property | Type | Description |
|---|---|---|
| Server Name | String | Name shown in Syphon clients |
| Enabled | Bool | Start/stop publishing (live) |
| Capture Mode | Enum | Main View (exact camera) / Scene Capture (independent) — switchable live |
| Camera Actor | Actor | Camera to publish |
| Set View Target | Bool | Main View: keep Camera Actor as player 0's view target. Turn off if your own logic drives the view target |
| Apply Resolution To Window | Bool | Main View, Standalone/packaged only: resize the window to Resolution |
| Resolution | Enum | Scene Capture: output size. Main View: window size used by Apply Resolution To Window. 1280×720, 1920×1080, 2560×1440, 3840×2160, Custom |
| Custom Resolution | IntPoint | Used when Resolution = Custom |
| Match Camera Aspect | Bool | Scene Capture: height follows the camera's aspect ratio |
| Force Opaque Alpha | Bool | Publish with alpha = 1 (default on). Unreal's output alpha isn't real transparency, and in Main View it is 0 |
| Show Debug | Bool | On-screen status messages (camera, mode, output size, widget count) |
| Render Widgets | Bool | Draw the overlay widgets into the Syphon image (live) |
| Overlay Widget Classes | Widget class array | Widgets created at Begin Play for the Syphon image only |

| Function | Description |
|---|---|
| Start Server / Stop Server | Start or stop publishing |
| Set Camera | Retargets the output to another camera at runtime (Main View: also sets the view target) |
| Set Capture Mode | Switch modes at runtime |
| Set Server Name | Rename (restarts the Syphon server) |
| Is Server Running | Pure query |
| Add Overlay Widget | Create a widget of a class for the Syphon image; returns it |
| Add Overlay Widget Instance | Draw an existing widget (on screen or not) into the Syphon image |
| Remove Overlay Widget / Clear Overlay Widgets | Stop drawing one or all overlay widgets |
| Get Overlay Widgets | The current overlay widget instances |

## Known limitations (v1.0)

- **One server per app** — the server is a singleton; multiple SyphonServer components will conflict
- **Publishes during Play (PIE) and in packaged builds** — no output while idle in the editor
- **Server only** — receiving Syphon sources *into* UE5 (client) is planned for v2
- **Overlay widgets are display-only** — they don't receive mouse or keyboard input through Syphon. A widget that is also on screen is drawn twice per frame; its Blueprint Tick runs once more with a delta time of 0
- Tested on UE 5.6 and 5.7 / Apple Silicon only

## How it works

A scene view extension hooks the end of the game view render. It copies either the viewport's final image (Main View) or the Scene Capture's render target (Scene Capture) into a small texture ring on the GPU, and the native Metal texture is handed to a `SyphonMetalServer` on Unreal's own Metal command queue, which shares it with other apps via IOSurface. Overlay widgets are rendered each frame into a transparent texture with Unreal's widget renderer and blended over the copy before it is published. With Force Opaque Alpha on, a tiny Metal pass first writes the frame to a BGRA8 texture with alpha = 1. All copies happen on the GPU; nothing is read back to the CPU.

## Licence

SyphonLink is released under the [MIT licence](LICENSE).

This plugin embeds the [Syphon framework](https://github.com/Syphon/Syphon-Framework), © bangnoise (Tom Butterworth) & vade (Anton Marini), used under its licence — see [LICENSE-Syphon](Source/SyphonLink/ThirdParty/Syphon/LICENSE-Syphon.txt).

## Credits
Built on the [Syphon framework](https://github.com/Syphon/Syphon-Framework)
by bangnoise (Tom Butterworth) & vade (Anton Marini)
Huge thanks to the Syphon community.