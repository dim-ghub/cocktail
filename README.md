# soberfps

Change the FPS cap of Roblox running under Sober (`org.vinegarhq.Sober`) while it's running.

`fpslimit_layer.c` is a small Vulkan layer that waits in `vkQueuePresentKHR` until the next
frame is due. It reads the cap from `~/.var/app/org.vinegarhq.Sober/cache/sober-fpscap`
every 50 ms, and `soberfps` writes that file.

Sober crashes if a Vulkan layer's library is loaded from a writable path (anything under
`~/.var/app`, even an unmodified vkBasalt), so the layer is packaged as a Flatpak
`org.freedesktop.Platform.VulkanLayer` extension. It is mounted read-only at
`/usr/lib/extensions/vulkan/SoberFps`, the same way vkBasalt is.

```
make install      # build in org.gnome.Sdk//50, install the extension bundle,
                  # set SOBER_FPSLIMIT=1 for Sober, install ~/.local/bin/soberfps
soberfps 144      # cap at 144
soberfps off      # no layer cap
soberfps          # show current caps
make uninstall
```

Patterns run in the foreground and put the previous cap back on Ctrl-C:

```
soberfps random 144            # random cap 1..144, new one every second
soberfps random 144 30 -e 0.5-3  # 30..144, random 0.5–3 s between changes
soberfps seq 1 12 123          # 1 → 12 → 123 → 1 → ...
soberfps seq 30 60 --once      # step through once, keep the last cap
soberfps ramp 10 120 5         # 10, 15, ... 120, then back down, repeating
soberfps wave 30 144 20        # smooth sine between 30 and 144, 20 s per cycle
```

`-e/--every SECS` (or `LO-HI` for a random delay) sets the time between changes, and
`-q` hides the live readout. The layer checks for a new cap every 50 ms, even partway
through a long frame, so very low caps like 1 fps switch away immediately.

The layer only runs in apps with `SOBER_FPSLIMIT=1` (set for Sober by a `flatpak override`).
It can only *lower* the frame rate below Roblox's own `FramerateCap` in
`GlobalBasicSettings_13.xml`.

Debugging: `flatpak run --env=SOBER_FPS_STATS=1 org.vinegarhq.Sober` prints the measured fps
once a second.
