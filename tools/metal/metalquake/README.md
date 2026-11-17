# MetalQuake on the paravirtual GPU

[MetalQuake](https://github.com/aloysha976/MetalQuake) (GPL-2.0-or-later) runs on the card through the paravirtual route
with one patch, `0001-bindless-world-only-with-128-textures.patch` (GPL-2.0-or-later, as the code it changes):
the static world is drawn by its bindless shader (`world_static_fragment_main`, 96 + 32 textures) only on devices that
guarantee 128 textures per stage (Mac GPU family 2, Apple family 6 and later); others take the per-texture world shader.
The paravirtual device reports neither family, and the host's tables hold 16 textures a stage.

Build for the rig (Intel), on a Mac with Xcode's Metal toolchain:

    git apply 0001-bindless-world-only-with-128-textures.patch
    xcodebuild -scheme MetalQuake -configuration Release ARCHS=x86_64 ONLY_ACTIVE_ARCH=NO \
        CODE_SIGN_IDENTITY=- CODE_SIGN_STYLE=Manual DEVELOPMENT_TEAM= build

Start it from its launcher (Browse to the folder that holds `id1`). `-engine` starts the engine before the app has
finished launching: from a shell its window never comes up.

Card (Tahoe 26.6.2, 2026-11-17): the demo and e1m1 at 3840x1878 in HDR, 60 frames a second on the display.
