# XHYN V upscaler (Snapdragon GSR 1) — experimental

A Vulkan layer that upscales the game's image with **Snapdragon Game Super Resolution 1**
(Qualcomm, BSD-3-Clause), so the game can render at a lower resolution and still fill the display.

**Status: not tested on a phone.** The Vulkan side is tested here on a software driver with the
validation layer (see [Tests](#tests)). Nothing has run on Android, Adreno or the real game yet.
Treat the first on-device run as a diagnostic: it starts in `probe` mode, which only logs.

## What it is not

- **Not FSR 3, FSR 4 or SGSR 2.** Those are temporal upscalers. They need the engine's depth buffer,
  per-pixel motion vectors and camera jitter every frame. This layer only receives the finished
  colour image, so there is nothing to feed them. FSR 4 additionally needs AMD RDNA 4 hardware.
- **Not frame generation.** That is a separate piece of work planned after upscaling is proven on a
  device. It would be colour-only interpolation, not FSR 3.

## How it works

`libgtav_vulkan.so` loads `libgtav_bcn.so` by name and calls three exported functions from it. This
project provides a replacement `libgtav_bcn.so` (the **shim**) with the same three exports:

```text
DXVK → libgtav_vulkan.so → shim (libgtav_bcn.so) → original BCn layer (libxhyn_bcn.so) → driver
                              │
                              └─ swapchain + vkQueuePresentKHR hooks → SGSR 1
```

- `scripts/install_shim.py` renames the original layer to `libxhyn_bcn.so` (patching its SONAME, 4
  bytes) and installs the shim in its place. Nothing else in the APK changes.
- The shim forwards everything. It only intercepts swapchain creation and presentation.
- In `sgsr1` mode the game gets images of **its own size** from a virtual swapchain; the real
  swapchain is larger. Each present runs one SGSR 1 fragment pass from the game's image into the
  real image. Image indices map 1:1, so acquire, semaphores and the swapchain handle are untouched.
- Only 8-bit SDR RGBA/BGRA swapchains (UNORM or sRGB) are upscaled. Anything else (HDR, 16-bit, other
  formats) is passed through unchanged and the reason is logged.

### BC textures: why `bcn_decode_mode` has to be On

The game only loads `libgtav_bcn.so` when BC decoding is forced (`on`) or when a GPU lacks BC
support. Adreno has hardware BC, so the default (`auto`) never loads the shim. Set **BC texture
decoding to On** in XHYN V's graphics settings (`bcn_decode_mode=on` under `[vulkan]` in `android.ini`) so the file is loaded. With `bcn = auto` (the
default here) the shim then keeps your GPU's hardware BC and does **not** route textures through the
original software-decode layer; it only goes through the original layer if some GPU lacks native
BC. Use `bcn = inner` to force the original behaviour.

## Build

You do **not** need the private base APK: `install_shim.py` works on the public release APK.

1. **The shim** (arm64-v8a), one of:
   - `ANDROID_NDK_HOME=/path/to/ndk modifications/upscaler/scripts/build_android.sh` (NDK r26+, cmake, ninja)
   - or run the *Build upscaler layer* workflow on GitHub and download the
     `xhyn-upscaler-libgtav_bcn` artifact.
2. **A signing key** (once): `keytool -genkeypair -keystore test.p12 -storetype PKCS12 -alias xhyn -keyalg RSA -keysize 2048 -validity 10000`
   and put the password in a text file. Android refuses to update an app signed with a different key,
   so first run `adb uninstall com.xhynph.xhynv.reborn174` (this removes the app's own settings and
   caches; your game folder is separate).
3. **The APK:**

   ```bash
   python modifications/upscaler/scripts/install_shim.py \
     --apk releases/1.7.4-en/XHYN-V-1.7.4-English.apk \
     --shim modifications/upscaler/build/android/libgtav_bcn.so \
     --out XHYN-V-sgsr.apk \
     --apksigner /path/to/android-sdk/build-tools/<version>/apksigner \
     --keystore test.p12 --key-alias xhyn --password-file password.txt
   ```

   The tool checks the shim (AArch64, SONAME, 16 KiB alignment, the three exports) and the original
   layer's SHA-256, refuses an APK that already has the shim, and writes a new file.

## Test on your phone with ADB

Turn on **Developer options → USB debugging** on the phone, connect it, run `adb devices` and accept the
prompt.

```bash
adb install XHYN-V-sgsr.apk
adb shell mkdir -p /sdcard/Android/data/com.xhynph.xhynv.reborn174/files
adb push modifications/upscaler/xhyn-upscale.conf.example \
  /sdcard/Android/data/com.xhynph.xhynv.reborn174/files/xhyn-upscale.conf
```

If the push is refused, push to `/sdcard/xhyn-upscale.conf` instead (the launcher has all-files access).

In the game's graphics settings set **BC texture decoding → On** and close the app completely.

**Run 1 — probe (changes nothing):** with `mode = probe` start the game and play for a couple of
minutes while logging:

```bash
adb logcat -c
adb logcat -v time -s XHYN-UPSCALE GTAV > probe.log
```

Stop with Ctrl+C and keep `probe.log`. What it should show, in order:

| Log line | Meaning |
|---|---|
| `BC decode: forced bcn_layer…` (tag GTAV) | the game loaded the shim's slot |
| `instance created, mode=probe, bcn=auto: hardware BC…` | the shim is running |
| `device created: Adreno …` | the GPU the layer sees |
| `swapchain create: WxH format=… ` and `surface: current=…` | **the key facts**: the size and format the game asks for |
| `[probe] frame N: … avg X ms (Y fps)` | frame pacing every 300 presents |

If there are no `XHYN-UPSCALE` lines at all, the shim was not loaded: check the BC setting and look
for `BC decode:` lines.

**Run 2 — upscale:** set `mode = sgsr1`, `scale = 1.5`, and lower the game's own resolution to
about 1/1.5 of what you want on screen (the layer upscales whatever size the game renders at). Look for
`sgsr1 ready: 960x540 -> 1440x810` and the `upscale GPU … ms` figure in the statistics line. If it
says `not applied: …` the reason is in that line. To inspect the picture, set `dump_frame = 600`,
play past that frame, then:

```bash
adb pull /sdcard/Android/data/com.xhynph.xhynv.reborn174/files/xhyn-upscale-in-600.ppm
adb pull /sdcard/Android/data/com.xhynph.xhynv.reborn174/files/xhyn-upscale-out-600.ppm
```

**Undo:** set `mode = off` (the shim then only forwards), or reinstall the original APK.

If something fails, send the `adb logcat` output from launch to the failure (`adb logcat -d > crash.log`
right after a crash). The layer logs every decision and every failed Vulkan call.

## Settings

`xhyn-upscale.conf` is read from, in order: `$XHYN_UPSCALE_CONF`, the app's external files folder
(`/sdcard/Android/data/<app id>/files/`), its internal files folder, then `/sdcard/xhyn-upscale.conf`.
`XHYN_UPSCALE_<KEY>` environment variables override the file (used by the host tests). The file is
re-read each time the game creates a swapchain.

| Key | Default | Meaning |
|---|---|---|
| `mode` | `probe` | `off` forwards only; `probe` logs only; `sgsr1` upscales |
| `scale` | `1.5` | output = game size × scale, 1.0–4.0; capped by the surface's maximum size |
| `out_width`, `out_height` | unset | explicit output size (both required); overrides `scale` |
| `edge_sharpness` | `2.0` | SGSR 1 sharpening, 1.0–2.0 |
| `edge_threshold` | `8` | SGSR 1 edge threshold in 1/255 steps |
| `edge_direction` | `false` | SGSR 1 edge-direction variant (slightly better edges, small extra cost) |
| `bcn` | `auto` | `auto`, `inner` (always the original layer) or `native` (never) |
| `log_interval` | `300` | presents between statistics lines, 0 disables |
| `gpu_timing` | `true` | measure the GPU time of the upscale pass |
| `dump_frame`, `dump_dir` | `0` | write input/output PPM at that present number |

## Known limits

- Presenting with `VkPresentRegionsKHR` (incremental present) is not rescaled; DXVK does not use it.
- A present that mixes several swapchains is handled, but only single-swapchain use is tested.
- The game's image is left in `PRESENT_SRC_KHR` layout, as for any presentable image; this is used
  on images that are not driver swapchain images, which the validation layer accepts but a driver may
  treat differently. This is the first thing to suspect if the picture is black or corrupted.
- HDR output (`hdr=true`) is not upscaled; the swapchain format is logged and passed through.
- If the probe log shows a swapchain `pNext sType=1000275000` (present scaling), the game asked for a
  scaling behaviour the layer does not adjust. Send the log; that is a one-line change but needs a
  device to confirm.

## Tests

`test/run_host_tests.sh` builds the shim and runs it against lavapipe (software Vulkan) with the
Khronos validation layer. A harness plays `libgtav_vulkan.so` and DXVK: it builds the same loader
link chains, creates a swapchain, renders into the images the shim gives it and presents. A stand-in
for the original BCn layer follows the same protocol (consumes and advances the link chain). Covered:

- layer protocol, pass-through, probe and BC policy decisions (hardware BC, no hardware BC, forced)
- SGSR 1 on all four 8-bit formats, **exact colour preservation** (flat colour in = flat colour out)
  for UNORM and sRGB, the edge-direction variant, resize/recreate, explicit output size
- swapchain/frame resource cleanup, 0 validation warnings or errors
- the AArch64 forwarding trampoline assembles to the intended three instructions

```bash
sudo apt-get install cmake ninja-build glslang-tools libvulkan-dev vulkan-validationlayers mesa-vulkan-drivers python3-pil
modifications/upscaler/test/run_host_tests.sh
```

Not covered: Android, Adreno/Mali drivers, the real game, performance, the Android build itself
(there was no NDK available when this was written; the CI job and `build_android.sh` have not run).

After editing `shaders/*.glsl`, run `scripts/gen_shaders.py` (needs `glslangValidator`); the test script
fails if `src/shaders_spv.h` is stale.

## Licensing

New code here is MIT ([LICENSE](LICENSE)). `shaders/sgsr1.frag` is derived from Qualcomm's
[Snapdragon GSR](https://github.com/SnapdragonGameStudios/snapdragon-gsr) (BSD-3-Clause, revision in
`third_party/snapdragon-gsr/REVISION`; the license text is kept in `third_party/snapdragon-gsr/` and
`notices/Snapdragon-GSR-BSD-3-Clause.txt`). The original BCn layer and the rest of the APK are
unchanged and keep their own terms; the release APK is an input, not part of this source.
