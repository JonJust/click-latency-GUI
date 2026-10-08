# Click Latency

A small C++17 Linux GUI for comparing a mouse's acoustic click with its
mouse-button press or release event. It uses Dear ImGui, SDL3 microphone capture
and input, and OpenGL. Dear ImGui and SDL3 source are included in `third_party/`, so building
does not download dependencies.

## Build on Ubuntu

Install the compiler, build tools, and desktop/audio development packages:

```bash
sudo apt update
sudo apt install build-essential cmake ninja-build pkg-config \
    libgl1-mesa-dev libx11-dev libxcursor-dev libxi-dev libxrandr-dev \
    libxfixes-dev libwayland-dev libxkbcommon-dev wayland-protocols \
    libdecor-0-dev libasound2-dev libpulse-dev libpipewire-0.3-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/click_latency
```

SDL detects the installed X11/Wayland and PipeWire/PulseAudio/ALSA headers when
CMake configures the build. The package list enables the common Ubuntu backends.
An X11-only build can use `build-essential cmake ninja-build pkg-config
libgl1-mesa-dev libx11-dev` plus at least one audio backend's development package;
PipeWire is recommended for current Ubuntu desktop installations. After adding
backend packages, configure in a fresh build directory so SDL redetects them.

## Take a measurement

1. Select your microphone in the audio input dropdown. **System default
   microphone** follows the system's default recording input.
2. Place the microphone close to the mouse. Watch the current dB reading under
   the threshold slider while the room is quiet, then while clicking the mouse.
3. Set the threshold above ambient noise and below the mouse's click peak.
   The initial setting is **−30 dBFS**. Move it toward 0 for a louder required
   transient, or toward −96 to detect quieter sounds.
4. Place the cursor over the large button and physically press the left mouse
   button. The microphone's first threshold crossing starts the trial; the
   mouse-down event over the button completes it. Tick **Measure release latency**
   to measure the release sound to the matching mouse-up event as well. Keep the
   cursor over the button when starting the press; its release remains tracked
   if the cursor moves away.
5. Read the latency in milliseconds. If no qualifying button press occurs in
   the **80 ms** window, the press result reads **no click detected**. Release
   timing has its own 80 ms window and result; a missed release reads
   **no release detected**. With release measurement disabled,
   release sounds are suppressed so they do not replace a successful press with
   a missed-click result.
6. Repeat the test to collect samples. The press graph shows individual successful
   measurements and their cumulative average. With release measurement enabled,
   a second graph shows the release measurements and average separately. Each
   series reports its sample count, average, minimum, maximum, and range
   (**maximum − minimum**). Timeouts are excluded from these statistics.

Results stay visible until the next corresponding trial. The detector requires
at least 20 ms of sound below the threshold minus 6 dB to rearm. An uncompleted
measurement keeps its full 80 ms window. After a successful measurement, the next
physical edge or press cycle can begin sooner once the quiet interval has elapsed.
After mouse-up, this quiet interval starts at that event. This avoids treating
one noisy click as several trials. Pause briefly between clicks.

**Reset averages** clears both series and starts a fresh measurement without
changing the microphone, threshold, or release setting. Graphs display the most
recent 256 successful samples per series; averages and summary statistics include
all successful samples since this reset. **Reset** clears the current results;
changing the threshold, microphone, or release checkbox also clears the current
trial. These actions preserve the collected averages until **Reset averages**
is used.

## Export labeled graphs

1. Collect successful samples for one mouse. Use **Reset averages** before
   testing a different mouse so each report represents a separate batch.
2. Click **Export PNG**, next to Reset averages. It captures the graphs and
   statistics at that moment.
3. Enter a **Mouse / graph label**, such as `Mouse A - wired, 1000 Hz`.
   This label is printed on the image and used to suggest its filename.
4. Click **Browse...** beside **Save PNG to** to choose an output folder in the
   system file explorer, then click **Save PNG**. The app remembers your last
   folder for later exports and keeps up to eight **Recent folders** in RAM for
   quick selection. This history clears when you exit. Folders used by successful
   exports with a manually entered path are remembered too.
   The initial default is a timestamped file in the working directory's
   `exports/` folder. You can still edit the full path; missing folders are created
   and `.png` is added if omitted. Choosing another folder preserves a filename
   you have edited. Existing files are kept, with an error asking you to choose a
   new filename.

The native folder picker uses your Linux desktop's XDG file chooser portal,
or `zenity` when a portal is unavailable (`sudo apt install zenity`). If neither
is available, the export dialog shows the error and accepts a typed path.

The image includes the down-press graph, its running average, sample count,
average/minimum/maximum/range, microphone, threshold, and capture time. If release
measurement was enabled when the snapshot was captured, it also includes the
release graph and statistics. Both exported graphs use the same latency scale.
The report is rendered at 1200 × 860 pixels, or 1200 × 1200 with release enabled,
independently of the app's window size or scroll position.

Exporting preserves the collected samples. Editing the label or waiting in the
export dialog does not change the captured data. Only successful samples enter
the statistics, and exported graphs retain the same latest-256-sample limit as
the live graphs. PNG encoding is included in the vendored SDL library; no extra
image dependency is needed.

The displayed level is **dBFS**, a digital peak relative to full scale, with a
−96 dBFS floor. It is not calibrated sound-pressure level (dB SPL). For multiple
microphone channels, the detector takes the largest absolute sample across the
channels, so opposite stereo polarities do not cancel the click.

## Timing limits

The application measures the estimated microphone threshold-crossing time to
the SDL mouse-down or mouse-up event timestamp. It locates the onset inside an
audio block and retains recent input timestamps when input arrives before an
audio callback.
The button state at the estimated sound timestamp determines whether the sound
belongs to a press or release, including when the audio callback arrives later.
The timeout and displayed result use those timestamps rather than GUI frame time.

SDL capture here does not expose ADC hardware timestamps. The audio block's end
is estimated from callback arrival and queued sample duration, leaving microphone,
driver, and audio-server buffering as an uncalibrated offset. This can make the
reported interval shorter, or place the estimated sound onset after the input
event so that the trial times out. Microphone placement, input gain, noise
processing, threshold, audio buffering, and system load also affect the result.
Use consistent settings when comparing runs. The readings do not establish
calibrated switch actuation or USB latency.

If capture fails or the meter stays at its floor, check Ubuntu's Sound input
settings, choose the intended microphone, and reselect it in the application.
Device and capture errors are shown in the window.

## Tests

```bash
ctest --test-dir build --output-on-failure
```

To build and run only the measurement tests without graphics/audio packages:

```bash
cmake -S . -B build-core -DCLICK_LATENCY_BUILD_GUI=OFF
cmake --build build-core --parallel
ctest --test-dir build-core --output-on-failure
```

The deterministic tests cover threshold crossings, sample-position timing,
stereo peaks, presses before and after the onset, the exact 80 ms boundary,
delayed event delivery, delayed audio callbacks, timeout, persistent results,
quiet/hysteresis rearming, press/release cycles, release-sound suppression,
independent release timeouts, sample statistics, bounded graph history,
average resets, and invalid audio data. These tests use synthetic samples and
timestamps. Real microphone/mouse measurements have not
been validated against calibrated external hardware.

GUI builds also run PNG export tests using SDL's offscreen video driver. They
check both report sizes, PNG decoding, error handling, preservation of existing
files, folder selection with automatic or edited filenames, and restoration of
the original GUI and OpenGL state after saving. Folder history and picker tests
cover RAM-only recent locations, default-folder reuse, collisions, cancellation,
picker errors, and callbacks that finish on another thread or after the owner closes.

Pinned dependency versions, licenses, and source archive checksums are recorded
in [third_party/README.md](third_party/README.md).

For a brief startup check, `./build/click_latency --smoke-test` opens the window,
reports the active video/audio backends and input count, and exits after 20
frames. Add `--screenshot /tmp/click-latency.bmp` to save the rendered window.
The smoke check also works without a desktop or microphone:

```bash
SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy ./build/click_latency --smoke-test
```

The Release build, deterministic measurement cases, and native startup
with X11 and PipeWire capture have been verified on the development Ubuntu
desktop. A fresh build directory can use `-G Ninja` for faster parallel builds.
