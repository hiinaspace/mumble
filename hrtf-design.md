# Mumble HRTF Positional Audio

## Overview

Replace Mumble's current positional audio spatialization (basic panning + distance attenuation + interaural time delay) with HRTF-based binaural rendering for significantly improved spatial perception on headphones.

**Approach**: Build a lightweight HRTF spatializer (~500-600 lines) using [FFTConvolver](https://github.com/HiFi-LoFi/FFTConvolver) (MIT, self-contained real-time convolution library) for the DSP and `libmysofa` for loading standard SOFA HRTF files. No Steam Audio dependency.

**Scope**: Client-side only. No server or protocol changes required.

**Licenses**: All new dependencies are compatible with Mumble's BSD-3-Clause license.
- FFTConvolver: MIT
- libmysofa: BSD-3-Clause
- Default HRTF dataset: public domain (CIPIC) or Apache 2.0 (SADIE II)

---

## Current Architecture

### What Mumble Transmits (Protocol)

The UDP audio packet (`MumbleUDP.proto`) carries:
```protobuf
repeated float positional_data = 6;  // [x, y, z] in meters
```

**Position only, no orientation/heading.** This is embedded in each audio packet by the sender if `bTransmitPosition` is enabled.

The TCP `UserState` message carries `plugin_context` (bytes) and `plugin_identity` (string) for server-side filtering -- the server only relays positional audio between clients with matching contexts.

### What Plugins Actually Provide (Client-Side)

The plugin API (`MumblePlugin.h::mumble_fetchPositionalData`) provides much richer data that is available locally but never fully transmitted:

| Field | Type | Description |
|-------|------|-------------|
| `avatarPos` | float[3] | Player position (meters) |
| `avatarDir` | float[3] | Player facing direction (unit vector) |
| `avatarAxis` | float[3] | Player up vector (unit vector) |
| `cameraPos` | float[3] | Camera position |
| `cameraDir` | float[3] | Camera facing direction |
| `cameraAxis` | float[3] | Camera up vector |

The **listener's** camera orientation (cameraDir + cameraAxis) is already used client-side for spatialization. We only know remote players' positions, not their orientations -- but this is sufficient for HRTF spatialization (see "The Orientation Problem" below).

### Current Spatialization (`AudioOutput.cpp`)

The current mixing loop (lines ~530-770):
1. Gets listener camera orientation (cameraDir, cameraAxis) from local plugin
2. Builds listener coordinate frame: `right = cameraAxis × cameraDir`
3. Rotates virtual speaker positions into listener frame
4. For each remote audio source:
   - Computes direction vector from listener to source position
   - Computes distance for attenuation (`calcGain()` -- logarithmic falloff, configurable min/max distance)
   - Computes dot product with each speaker direction for per-channel volume
   - Applies interaural time delay (ITD): ~0.43ms max offset at 48kHz (~20 samples)
   - Linearly interpolates volume and ITD offset across frame to avoid clicks

This produces a basic "the sound is over there" effect but lacks the spectral shaping of a real HRTF, resulting in poor elevation perception and weak front/back disambiguation.

---

## HRTF Background

An HRTF (Head-Related Transfer Function) captures how sound from a specific direction is filtered by the listener's head, ears, and torso before reaching each eardrum. The time-domain version (HRIR -- Head-Related Impulse Response) is a short filter (~200 samples at 48kHz, ~4ms) measured for each ear at each direction.

To spatialize a mono audio source:
1. Look up the HRIR pair (left ear, right ear) for the source's direction relative to the listener
2. Convolve the mono audio with each HRIR independently
3. The resulting stereo output sounds like the source is at that direction

The convolution is efficiently implemented in the frequency domain using overlap-add:
1. FFT the input audio frame
2. Multiply by the pre-FFT'd HRIR for each ear
3. Inverse FFT
4. Overlap-add with the previous frame's tail

This is textbook DSP -- the only non-trivial parts are the HRTF data itself and the direction-to-HRIR lookup.

---

## Design

### Approach: FFTConvolver + libmysofa + SOFA HRTF

Rather than linking Steam Audio (a large library with many features we don't need), we use two focused libraries:

- **FFTConvolver** (MIT, 8 source files): A proven real-time convolution library used widely in audio plugins. It handles all the DSP: FFT, overlap-add, internal buffering, and edge cases. Its streaming API (`init()` → `process()` → `reset()`) maps directly to our per-source, per-ear convolution needs.
- **libmysofa** (BSD-3): Loads SOFA HRTF files, handles resampling and nearest-neighbor spatial lookup.

The key insight from studying Steam Audio's implementation is that the binaural effect is architecturally simple:
- **Steam Audio's `BinauralEffect::apply()`**: ~100 lines -- looks up an HRTF, calls overlap-add convolution
- **HRTF lookup**: delegate to libmysofa's `mysofa_lookup()` (nearest-neighbor) which handles the coordinate transforms and spatial indexing

**What we write ourselves**: Glue to load HRTFs via libmysofa and manage FFTConvolver instances with double-buffering (~250 lines), integration into AudioOutput::mix with stereo downmix and interleaved output (~150 lines), settings/persistence wiring (~50 lines), coordinate mapping and validation (~50 lines). Total: **~500-600 lines of new code**.

**What we don't write**: Convolution (FFTConvolver), HRTF data parsing (libmysofa), spatial nearest-neighbor lookup (libmysofa). These are the parts with subtle edge cases.

### Default HRTF Dataset

We ship a standard SOFA file as the default HRTF. Two strong candidates:

**CIPIC Subject 124 (KEMAR mannequin)** -- Recommended
- Source: UC Davis CIPIC database
- License: **Public domain** -- no attribution required, no redistribution restrictions
- Subject: KEMAR dummy head (designed to represent average human acoustics)
- Measurement positions: 1,250 directions (25 azimuths × 50 elevations)
- Sample rate: 44.1kHz (libmysofa resamples to 48kHz automatically)
- Industry validation: Steam Audio uses this exact dataset as its built-in default
- Estimated SOFA file size: ~1-2 MB

**SADIE II D1 (KU100 dummy head)** -- Alternative
- Source: University of York
- License: Apache 2.0 (requires attribution, compatible with BSD)
- Subject: Neumann KU100 dummy head (professional reference microphone)
- Measurement positions: 1,550+ directions
- Sample rate: 96kHz (higher quality source, resampled to 48kHz)
- Higher measurement density and potentially better quality
- Estimated SOFA file size: ~2-4 MB

**Recommendation**: Ship **CIPIC subject 124** as the default. Public domain licensing is simplest for an open-source project, and it's proven by Steam Audio's widespread use. The SADIE II KU100 can be mentioned in documentation as a recommended alternative for users who want to try a different HRTF.

Because we use standard SOFA files, users can also substitute any compatible SOFA HRTF (personal measurements, other datasets) by pointing the setting at a different file.

### Architecture

```
                    ┌─────────────────────────────────┐
                    │         AudioOutput::mix()       │
                    │                                  │
                    │  for each source:                │
                    │    decode opus → mono float buf  │
                    │           │                      │
                    │    ┌──────┴───────┐              │
                    │    │ if HRTF on   │              │
                    │    │              │              │
                    │    ▼              ▼              │
                    │  HrtfSpatializer Legacy path     │
                    │  (our convolver)  (existing code) │
                    │    │                             │
                    │    ▼                             │
                    │  stereo HRTF  ──► mix into       │
                    │  output          output buffer   │
                    └─────────────────────────────────┘
```

### New Class: `HrtfSpatializer`

```cpp
// src/mumble/HrtfSpatializer.h

#include <FFTConvolver.h>

class HrtfSpatializer {
public:
    // Initialize with default or custom SOFA file.
    // mixerFreq is the audio backend's actual output rate (iMixerFreq),
    // which may differ from Opus's 48kHz decode rate.
    // blockSize is the maximum expected frame count from the audio backend.
    explicit HrtfSpatializer(int mixerFreq, int blockSize);
    ~HrtfSpatializer();

    // Load an HRTF from a SOFA file. Empty path = default shipped HRTF.
    // libmysofa resamples the HRIR data to mixerFreq automatically.
    // Returns false on failure (bad file, unsupported format, etc).
    bool loadHRTF(const QString &sofaPath = {});

    // Spatialize a mono source buffer into interleaved stereo output.
    // direction: unit vector from listener to source in listener-local coords.
    // outInterleaved: output buffer, frameCount * 2 floats (L0 R0 L1 R1 ...).
    // frameCount: number of frames this callback (may vary between calls).
    void spatialize(
        unsigned int sessionId,
        const float *monoIn,
        float *outInterleaved,
        unsigned int frameCount,
        const Vector3D &direction
    );

    // Clean up state for a disconnected user.
    void removeSource(unsigned int sessionId);

    bool isLoaded() const;

private:
    // SOFA / HRTF data
    MYSOFA_EASY *m_sofa = nullptr;
    int m_numSamples = 0;        // HRIR length in samples

    // Raw HRIR data extracted from SOFA at load time.
    // Layout: [numHRIRs][2 ears][numSamples]
    // Stored as time-domain impulse responses -- FFTConvolver handles
    // the frequency-domain transform internally.
    std::vector<std::vector<std::vector<float>>> m_hrirs;

    // Per-source convolution state (keyed by session ID).
    // Uses double-buffered convolvers to allow realtime-safe HRTF switching:
    // the "active" pair runs in the audio callback, while the "pending" pair
    // can be pre-initialized with a new HRIR without blocking audio.
    struct SourceState {
        fftconvolver::FFTConvolver convolverL[2];  // [0]=active, [1]=pending
        fftconvolver::FFTConvolver convolverR[2];
        int activeSlot = 0;
        int currentHrtfIndex = -1;
        int pendingHrtfIndex = -1;
        int crossfadeSamples = 0;     // remaining crossfade frames (0 = not fading)
    };
    QHash<unsigned int, SourceState> m_sources;

    // Audio settings
    int m_mixerFreq;
    int m_blockSize;

    // Helpers
    int lookupDirection(const Vector3D &dir) const;
    void preparePendingConvolver(SourceState &state, int newIndex);
};
```

### Convolution via FFTConvolver

FFTConvolver handles all the overlap-add DSP internally. Per source, per ear, the basic API is:

```cpp
convolver.init(blockSize, hrir, hrirLength);   // one-time setup (allocates)
convolver.process(monoInput, output, numSamples); // streaming, per-callback
convolver.reset();                              // on disconnect
```

FFTConvolver automatically selects an efficient internal FFT size, manages overlap buffers, and handles arbitrary block sizes and IR lengths. Its `process()` call is streaming -- it maintains internal state across calls, so each invocation picks up exactly where the last left off. `process()` also accepts variable `numSamples` per call, which is important since Mumble's audio backends pass varying frame counts to `mix()`.

**Realtime-safe HRTF switching (double-buffer)**: `FFTConvolver::init()` allocates memory and must not be called in the audio callback. To handle direction changes safely, each source maintains two pairs of convolvers (slots 0 and 1). The active slot runs in the audio callback via `process()`. When `mysofa_lookup()` returns a new HRTF index, the inactive slot is initialized with the new HRIR on a worker thread (or pre-initialized lazily outside the callback). Once ready, the audio callback crossfades from the active to the pending slot over one frame, then swaps. This avoids any allocation in the realtime path.

For sources whose direction changes slowly (the common case), the active convolver just keeps running -- no switching overhead at all.

### HRTF Loading via libmysofa

At initialization:
```
1. mysofa_open(sofaPath, mixerFreq, &numSamples, &status)
   → libmysofa loads the SOFA file, resamples HRIRs to the mixer's sample rate
   (which may differ from Opus's 48kHz decode rate -- see "Sample Rate" below)
2. For each measurement position i (0..M-1):
   a. Extract left/right HRIR from mysofa data arrays
   b. Store in m_hrirs[i][0] (left) and m_hrirs[i][1] (right)
   (FFTConvolver handles the FFT internally when init() is called)
3. At runtime, mysofa_lookup() returns the nearest measurement index
   for a given direction → index into m_hrirs → used to init the pending convolver slot
```

**Sample rate**: Mumble decodes Opus at 48kHz but resamples to the audio backend's native rate (`iMixerFreq`) before mixing. The HRTF convolution operates in the mix path, so it must run at `iMixerFreq`, not 48kHz. `mysofa_open()` accepts the target sample rate and resamples HRIRs automatically.

**SOFA validation**: Some SOFA files use non-zero `DataDelay` fields (per-measurement timing offsets). Steam Audio explicitly rejects these. Our implementation should either handle `DataDelay` or reject such files with a clear error message, since ignoring it produces timing artifacts.

libmysofa handles:
- SOFA/HDF5 file parsing
- HRIR resampling to target sample rate
- Coordinate system conversion (SOFA uses a specific spherical convention)
- Nearest-neighbor spatial lookup via `mysofa_lookup()`
- Neighbor finding for potential interpolation via `mysofa_neighborhood()`

### Direction Computation

To compute the direction vector from listener to source in listener-local coordinates, we reuse the existing math from `AudioOutput::mix()`:

```cpp
// World-space direction from listener to source (already computed by existing code)
// connectionVec is normalized to unit length when len > 0

// Transform to listener-local coordinates
// Using the existing listener coordinate frame (right, cameraAxis, cameraDir)
Vector3D localDir;
localDir.x = dot(connectionVec, right);       // right component
localDir.y = dot(connectionVec, cameraAxis);   // up component
localDir.z = dot(connectionVec, cameraDir);    // forward component

// Convert to SOFA Cartesian coordinates for mysofa_lookup().
// SOFA uses: +x = front, +y = left, +z = up.
// Our listener-local frame uses: +x = right, +y = up, +z = forward.
// Steam Audio's conversion (sofa_hrtf_map.cpp:507): toSOFA(v) = (-v.z, -v.x, v.y)
// Adapted for our convention:
float sofaCoords[3] = { localDir.z, -localDir.x, localDir.y };

// mysofa_lookup expects Cartesian coordinates directly -- do NOT call mysofa_s2c()
// (that converts spherical→Cartesian, which is wrong when already in Cartesian).
int hrtfIndex = mysofa_lookup(m_sofa->lookup, sofaCoords);
```

**Important**: The coordinate mapping above is derived from Steam Audio's `toSOFACoordinates()` but adapted for Mumble's listener-local frame. It must be validated during implementation against known test directions:
- Front (0,0,1) → SOFA (1,0,0)
- Right (1,0,0) → SOFA (0,-1,0)
- Up (0,1,0) → SOFA (0,0,1)

This is one of the highest-risk areas for subtle bugs. The Steam Audio coordinate tests (`PolarVector.test.cpp`) should be adapted to validate our mapping.

### Integration into `AudioOutput::mix()`

The existing positional audio block in `AudioOutput::mix()` currently does per-channel gain + ITD calculations. With HRTF enabled, this is replaced.

**Important buffer layout details**: Mumble's `mix()` writes to a single interleaved float buffer: `output[i * nchan + channel]`. The number of channels (`nchan`) and frame count (`frameCount`) are determined by the audio backend and vary at runtime. HRTF output is stereo only, so HRTF mode should only activate when `nchan == 2` (headphone output) -- for surround configurations, the legacy path remains active.

**Input format**: Opus always decodes as stereo (`bStereo = true`). The existing positional path downmixes to mono inline during the per-channel mixing loop. For the HRTF path, we downmix to mono first, then convolve.

```cpp
// Existing code computes: connectionVec (listener→source), len (distance)
// buffer->fPos is checked for (0,0,0) sentinel before reaching here.

if (nchan == 2 && hrtfSpatializer && hrtfSpatializer->isLoaded()) {
    // HRTF path: compute direction in listener-local coordinates
    // using existing right/cameraAxis/cameraDir basis vectors
    Vector3D localDir;
    localDir.x = connectionVec.x * right.x + connectionVec.y * right.y + connectionVec.z * right.z;
    localDir.y = connectionVec.x * cameraAxis.x + connectionVec.y * cameraAxis.y + connectionVec.z * cameraAxis.z;
    localDir.z = connectionVec.x * cameraDir.x + connectionVec.y * cameraDir.y + connectionVec.z * cameraDir.z;

    // Downmix stereo Opus output to mono for HRTF input
    float mono[frameCount];
    for (unsigned int i = 0; i < frameCount; ++i)
        mono[i] = (pfBuffer[2 * i + currentOffset] + pfBuffer[2 * i + currentOffset + 1]) * 0.5f;

    // Spatialize mono → interleaved stereo HRTF output
    float hrtfOut[frameCount * 2];
    hrtfSpatializer->spatialize(sessionId, mono, hrtfOut, frameCount, localDir);

    // Apply Mumble's existing distance attenuation and mix into interleaved output
    float gain = calcGain(dot, len) * volumeAdjustment;
    for (unsigned int i = 0; i < frameCount; ++i) {
        output[i * nchan + 0] += hrtfOut[i * 2 + 0] * gain;  // left
        output[i * nchan + 1] += hrtfOut[i * 2 + 1] * gain;  // right
    }
} else {
    // Legacy path: existing per-channel panning + ITD code (unchanged)
}
```

**Key decisions**:
- Keep Mumble's existing distance attenuation model (`calcGain()`) and only use the HRTF for directional filtering. The HRTF tells you *where* the sound is; Mumble's existing code tells you *how loud* it is.
- HRTF only activates for stereo (2-channel) output. Surround configurations continue to use the existing multi-channel panning path.
- The `(0,0,0)` position sentinel (meaning "no positional data") is checked before this code runs, same as the existing path -- sources without positions skip spatial processing entirely.

### Per-Source State Management

Each remote user needs their own convolution state (FFTConvolver internal buffers). This is stored in `HrtfSpatializer::m_sources` keyed by session ID:

- **Created lazily** when a user first sends positional audio
- **Destroyed** when the user disconnects (`removeSource()`)
- **Convolvers reset** if there's a long gap in audio (to avoid stale tail ringing)

**Thread safety**: The `m_sources` map must be synchronized with Mumble's existing `qmOutputs` lifecycle. `AudioOutput::mix()` runs on the audio backend thread and is already lock-protected via `qrwlOutputs`. The simplest approach is to manage HRTF source state entirely within the `mix()` code path (same thread), creating/destroying entries at the same points where `AudioOutputBuffer` is created/destroyed. The `removeSource()` call should be wired into the existing `removeBuffer()` path. `FFTConvolver::init()` for the pending double-buffer slot can be called outside the lock, since it only writes to the inactive slot.

---

## The Orientation Problem

### Current State

The protocol transmits only `[x, y, z]` position per audio packet. No sender orientation.

For HRTF spatialization, **source orientation is not needed**. The HRTF is applied based on the direction from *listener* to *source*, which requires only:
- Source position (transmitted in each audio packet)
- Listener position and orientation (available locally from the plugin)

Source orientation would only matter for **directivity modeling** (voice quieter when speaker faces away). This is a secondary effect and a future enhancement -- not a blocker.

### Future Protocol Extension (Optional)

If we later want source directivity, the UDP proto can be extended:
```protobuf
repeated float orientation_data = 17;  // [ahead_x, ahead_y, ahead_z, up_x, up_y, up_z]
```
Backwards-compatible: proto3 ignores unknown fields. Bandwidth: ~1.2 KB/s per speaker.

---

## Dependencies

### FFTConvolver (vendored)

- **What**: Real-time partitioned convolution library
- **License**: MIT
- **Source**: https://github.com/HiFi-LoFi/FFTConvolver
- **Size**: 8 source files (~1500 lines total), self-contained (includes its own FFT implementation)
- **API surface used**: `FFTConvolver::init(blockSize, ir, irLen)`, `FFTConvolver::process(in, out, len)`, `FFTConvolver::reset()`
- **Why vendor**: Small, stable, no external dependencies. Vendoring avoids adding a system package dependency for 8 files.

### libmysofa (required)

- **What**: Lightweight C library for reading SOFA HRTF files
- **License**: BSD-3-Clause
- **System availability**: Packaged in Ubuntu (`libmysofa-dev`), Debian, Fedora, Arch, Homebrew
- **Transitive deps**: zlib only (Mumble likely already links zlib)
- **Size**: ~50KB installed headers + shared library
- **API surface used**: `mysofa_open()`, `mysofa_lookup()`, `mysofa_close()`, data access via `MYSOFA_EASY` struct

### Default HRTF SOFA file (shipped as data)

- **What**: CIPIC subject 124 KEMAR SOFA file
- **License**: Public domain
- **Size**: ~1-2 MB
- **Installed to**: Mumble data directory alongside other resources

---

## Build Integration

```cmake
# src/mumble/CMakeLists.txt additions:

option(hrtf "Build with HRTF binaural spatialization" ON)

if(hrtf)
    find_package(MySofa REQUIRED)  # or pkg_check_modules(MYSOFA REQUIRED libmysofa)
    target_link_libraries(mumble_client_object_lib PUBLIC ${MYSOFA_LIBRARIES})
    target_include_directories(mumble_client_object_lib PUBLIC ${MYSOFA_INCLUDE_DIRS})
    target_compile_definitions(mumble_client_object_lib PUBLIC USE_HRTF)

    target_sources(mumble_client_object_lib PRIVATE
        HrtfSpatializer.cpp
        HrtfSpatializer.h
        # Vendored FFTConvolver sources
        3rdparty/fftconvolver/FFTConvolver.cpp
        3rdparty/fftconvolver/AudioFFT.cpp
        3rdparty/fftconvolver/Utilities.cpp
    )
    target_include_directories(mumble_client_object_lib PRIVATE
        3rdparty/fftconvolver
    )
endif()
```

Install the default SOFA file:
```cmake
if(hrtf)
    install(FILES "${CMAKE_SOURCE_DIR}/data/hrtf/default.sofa"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/mumble/hrtf")
endif()
```

All HRTF code is guarded with `#ifdef USE_HRTF`.

---

## Settings / UI

### New Settings (`Settings.h`)

```cpp
bool bHrtf = false;                // Enable HRTF binaural spatialization
QString sHrtfFile;                 // Custom SOFA file path (empty = default)
```

### UI Changes

In Audio Output settings, under "Positional Audio":
- Checkbox: "Use HRTF binaural audio (headphones recommended)"
- Optional file picker: "Custom HRTF file (.sofa)" -- empty uses default

Minimal UI for Phase 1. The SOFA file picker is straightforward since the format is well-known and documented.

---

## Testing Strategy

### Unit Tests (new)

1. **FFTConvolver integration**: Convolve a known signal (impulse, sine wave) with a known HRIR via FFTConvolver. Verify output matches expected result within tolerance. (This validates our usage of the library, not the library itself.)
2. **HRTF loading**: Load the default SOFA file, verify measurement count and HRIR length are reasonable.
3. **Direction lookup smoke test**: Verify that cardinal directions (front, back, left, right, above) return distinct HRTF indices. This catches coordinate system mapping errors.
4. **Numerical stability**: Fuzz test with random directions (similar to Steam Audio's `BinauralEffect.test.cpp` which tests 10,000 random directions for NaN/infinity).

### Integration Tests

5. **End-to-end with manual positional plugin**: Enable HRTF, use Mumble's built-in manual positional audio plugin, verify spatial perception by ear. This is necessarily subjective but catches gross errors (wrong ear, inverted coordinates).

### Tests We Can Adapt from Steam Audio

Steam Audio uses Catch2. The most reusable tests:
- **`PolarVector.test.cpp`** (~468 lines): Pure-math coordinate transform tests for all cardinal directions. Can be adapted to validate our coordinate mapping.
- **`CoordinateSpace.test.cpp`** (~68 lines): Orthonormal basis tests. Directly applicable.
- **`BinauralEffect.test.cpp`**: The 10,000-random-direction fuzz test pattern is worth replicating.

The Steam Audio tests validate numerical stability (no NaN/inf) rather than golden-output correctness. For our convolver, a simple "convolve impulse with known HRIR, check output matches HRIR" test provides stronger correctness guarantees than anything Steam Audio tests.

---

## Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Coordinate system mapping error | Sound in wrong direction | Test cardinal directions explicitly; adapt Steam Audio's coordinate tests. Highest-risk area. |
| Sample rate mismatch | HRTF at wrong pitch/timing | Use `iMixerFreq` (not hardcoded 48kHz) for HRTF load and convolution. libmysofa resamples automatically. |
| `FFTConvolver::init()` in audio callback | Glitches / allocation stalls | Double-buffered convolvers: init pending slot outside callback, crossfade to swap. |
| libmysofa not available on platform | Build failure | Make dependency optional (`find_package` with fallback) |
| SOFA file missing at runtime | Feature silently disabled | Log warning, fall back to legacy spatialization |
| SOFA DataDelay non-zero | Timing artifacts | Reject or warn on SOFA files with non-zero DataDelay (following Steam Audio's approach) |
| Frame size varies across backends | Unexpected behavior | FFTConvolver::process() handles variable `numSamples` per call. No fixed frame size assumed. |
| Per-source memory overhead | High memory with many users | Each source: 4 FFTConvolver instances (~16KB, double-buffered). 50 users = 800KB. Negligible. |
| Thread safety of source map | Race conditions / stale state | Manage HRTF source lifecycle on same thread as `mix()`, tied to existing `AudioOutputBuffer` lifecycle. |
| Stereo input assumption | Wrong levels or channel handling | Opus always decodes stereo; downmix to mono before HRTF convolution, matching existing positional path. |

---

## Implementation Phases

### Phase 1: Minimal Viable HRTF
1. Vendor FFTConvolver into `3rdparty/fftconvolver/`
2. Add libmysofa as a build dependency (CMake `find_package`)
3. Ship CIPIC subject 124 SOFA file as default
4. Implement `HrtfSpatializer` class:
   - SOFA loading via libmysofa
   - Extract and store time-domain HRIRs
   - Per-source FFTConvolver pairs (left/right ear)
   - Per-source state management
5. Hook into `AudioOutput::mix()` -- replace per-source gain/ITD path when HRTF enabled
6. Keep existing distance attenuation model
7. Add settings toggle (`bHrtf`)
8. Basic tests: integration correctness, direction lookup, numerical stability
9. Test with manual positional audio plugin

### Phase 2: Polish
1. Custom SOFA file picker in UI
2. Smooth crossfade when HRTF index changes (avoid clicks on rapid direction changes)
3. Proper lifecycle: cleanup on disconnect, reload on settings change

### Phase 3: Protocol Extension (Optional, Future)
1. Add `orientation_data` to UDP proto
2. Transmit `avatarDir` + `avatarAxis`
3. Apply source directivity attenuation

---

## Key Files to Modify

| File | Change |
|------|--------|
| `3rdparty/fftconvolver/` | **New** -- vendored FFTConvolver (8 files, MIT) |
| `src/mumble/CMakeLists.txt` | Add libmysofa dependency, FFTConvolver sources, compile flag |
| `src/mumble/HrtfSpatializer.h` | **New** -- HRTF spatializer class |
| `src/mumble/HrtfSpatializer.cpp` | **New** -- implementation |
| `src/mumble/AudioOutput.cpp` | Integrate spatializer into mixing loop (~lines 530-770) |
| `src/mumble/AudioOutput.h` | Add `HrtfSpatializer` member |
| `src/mumble/Settings.h` | Add `bHrtf`, `sHrtfFile` settings |
| `src/mumble/AudioConfigDialog.cpp` | UI toggle for HRTF |
| `CMakeLists.txt` | Top-level `hrtf` option |
| `data/hrtf/default.sofa` | **New** -- shipped CIPIC KEMAR SOFA file |

---

## Open Questions

1. **Multi-channel gating**: HRTF only makes sense for stereo headphone output (`nchan == 2`). Should this be gated on the existing `bPositionalHeadphone` setting, or should we add a separate `bHrtf` toggle? If using `bPositionalHeadphone`, HRTF becomes opt-in via an existing known setting. If separate, users get finer control but more UI complexity.
2. **CIPIC SOFA availability**: The CIPIC database was originally distributed as MATLAB `.mat` files. Need to find or generate a SOFA conversion. The SOFA Conventions website hosts converted versions, or we can use MATLAB/Python tools to convert from the original data.
3. **Double-buffer init threading**: The pending FFTConvolver slot needs to be initialized outside the audio callback. Options: (a) a small worker thread that pre-inits on direction change, (b) init lazily during non-positional frames, or (c) accept a brief gap where the old HRTF continues playing until the next non-audio opportunity. Need to profile `FFTConvolver::init()` cost to determine if (c) is viable.
