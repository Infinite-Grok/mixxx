# PortAudio Oboe registration repair (recipe inputs)

Files here are the retained inputs of the repair; `../02_build_portaudio.sh` applies them in this order to the
pinned PortAudio source and checks every hash:

| file | origin |
|---|---|
| `patches/0001-Add-basic-support-for-iOS-to-portaudio.patch` | Hans Petter Selasky (PortAudio PR 1084 series), as carried by the Mixxx vcpkg overlay at `mixxxdj/vcpkg@1c20f84a` |
| `patches/0002-Update-CMakeLists-with-iOS-implementation.patch` | same |
| `patches/0003-Fix-renamed-memory-allocation-functions.patch` | same |
| `patches/0004-Add-Android-OBOE.patch` | the Oboe backend (PortAudio PR 1084) |
| `patches/0005-Fix-Oboe-registration-safety.patch` | **the repair** (AI-assisted, human reviewed): allocate `strlen + 1`, null-name and allocation-failure handling, ownership of the registered entry |
| `patches.sha256` | sha256 of the five patches (checked by the script) |

Base source: `PortAudio/portaudio@b0cc303e95fdb7c6c953337051378071c9043e88` (GitHub tarball, sha512
`fa7b4060...1c46`, the full value is in `02_build_portaudio.sh`). PortAudio is MIT licensed; the patches carry their
authors' headers.

After patches 0001-0004 `src/hostapi/oboe/pa_oboe.cpp` is byte-identical to PortAudio PR 1084's head; after 0005 its sha256
is `4295e67089691aed812b09c6530894200e025d97ceeefbde788d6ad83feeb28e` (checked by the script).

## Build options that matter (and why)

Beyond the original recipe, two things were needed to make the library usable by Mixxx:

- `-DPA_USE_ALSA=ON -DPA_USE_JACK=ON` with the dependency tree's `libasound.a` / `libjack.a`: Mixxx's
  `sounddeviceportaudio.cpp` references `PaAlsa_EnableRealtimeScheduling`, and the pinned archive's own
  `libportaudio.a` contains `pa_linux_alsa.c.o` and `pa_jack.c.o`. A library without ALSA fails at the final link
  (after about 20 minutes of compiling).
- `-DPA_USE_SNDIO=OFF` and `PKG_CONFIG_LIBDIR` limited to the dependency tree, because a host `libsndio` was
  auto-detected through the host pkg-config and broke the cross build.

## How the repair is known to be in the shipped app

The patched object imports `free` and no longer imports `strcpy`; in `OboeEngine::registerDevice` the machine code is
`strlen`, `+1`, `malloc`, `memcpy(len+1)`, `paBadBufferPtr` for a null name, and a `bad_alloc` handler. The same sequence
was found in the final `libmixxx_arm64-v8a.so` of the earlier candidate APKs. The A1 sanitizer harness (ASan/UBSan, lengths
0 to 255, null names, injected allocation failures) was run against the patch source, not against the final Android
binary. Registration at runtime on a device is not yet shown.
