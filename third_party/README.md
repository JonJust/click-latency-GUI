# Vendored dependencies

These source snapshots are included so configuring and building the application
does not need network access. Upstream source files are unchanged.

| Library | Pinned version | License | Official source archive | SHA-256 |
| --- | --- | --- | --- | --- |
| Dear ImGui | 1.92.9b | MIT | <https://codeload.github.com/ocornut/imgui/tar.gz/refs/tags/v1.92.9b> | `21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99` |
| SDL | 3.4.18 | zlib | <https://www.libsdl.org/release/SDL3-3.4.18.tar.gz> | `9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3` |

`imgui/` contains the four core implementation files, their headers, the SDL3
and OpenGL3 backends, their headers, the OpenGL loader, and `LICENSE.txt`. The
demo and examples are excluded. `SDL3/` contains the upstream source release.

SDL is built statically with its tests, examples, installation, and unrelated
subsystems disabled. Linux X11, Wayland, ALSA, PulseAudio, and PipeWire support
is detected from installed development headers. These platform libraries are
loaded dynamically where SDL supports it. See the main README for Ubuntu
development packages and build commands.
