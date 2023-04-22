# A Wayland GUI agent for Qubes OS

This is a GUI agent for [Qubes OS] that supports the [Wayland] display server protocol.
Compared to X11, Wayland is _vastly_ simpler and aims to ensure every frame is perfect.
Due to Qubes OS limitations, this agent does not achieve this, but it does its best.
Compatibility with legacy X11 programs is provided via Xwayland.

## Prerequisites

The Wayland agent uses [Meson] to build, so you must have Meson installed.
Additionally, `pkg-config` must be able to find `xkb`, `libdrm`, `wayland-server`, `xkbcommon`, `pixman-1`, and `vchan-xen`.
If it can find `libsystemd`, the compositor will integrate nicely with [systemd].
Parts of the agent are written in [Rust], so you also need to have Cargo installed.

In a Fedora 37 qube, you can install these with

```
sudo dnf install libxcb-devel libdrm-devel wayland-devel libxkbcommon-devel pixman-devel systemd-devel qubes-libvchan-xen-devel cargo
```

## Building

Build the Wayland agent like any other program that uses Meson:

```sh
cd /path/to/source && meson setup build && cd build && meson compile
```

The Rust components are built using Cargo, but this is handled internally by the build system and you do not need to worry about it.
If any of Cargo’s inputs have changed, Cargo should be rerun automatically; if it is not, this is a bug in Cargo.

## Running

If you use systemd, I recommand using a systemd user unit to start the compositor.
A sample unit file is provided.
Otherwise, it should be sufficient to run the compositor with no arguments.
Running `qubes-compositor --help` will provide detailed usage information; please report a bug if it is not sufficient.

The compositor and the standard agent cannot be run concurrently.
Whichever starts later will hang until the other has been stopped.
To prevent this problem, stop the standard agent first with `systemctl stop qubes-gui-agent.service`.
This will kill any graphical programs running in the VM, so it should be run from a console window opened with `qvm-console-dispvm` or the Console button in qubes-manager.

## Internals

The compositor uses [wlroots] for all Wayland-related functionality and for rendering.
Currently, all rendering is done via [Pixman] in software.
This means that even if the VM has a GPU attached to it (via e.g. GPU pass-through) it will not be used.
The code that processes messages from the GUI daemon is written in Rust, but the rest of the code is written in C due to the lack of good wlroots Rust bindings.

## Status

Demi Marie Obenour (the author of the compositor) does all development using Vim and gnome-terminal running under the compositor itself, so it is capable of running non-toy applications.
Nevertheless, it is still experimental and will have bugs.

Known bugs:

- Damage tracking for Xwayland surfaces doesn’t work right and has been disabled.
- If a window is resized quickly, there may be a bar on the right with stale contents
  until the application redraws.

[Rust]: https://rust-lang.org
[systemd]: https://systemd.io
[Meson]: https://mesonbuild.com
[Qubes OS]: https://www.qubes-os.org
[wlroots]: https://gitlab.freedesktop.org/wlroots/wlroots
[Pixman]: https://gitlab.freedesktop.org/pixman/pixman
