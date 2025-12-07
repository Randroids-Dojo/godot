# Godot Engine

<p align="center">
  <a href="https://godotengine.org">
    <img src="logo_outlined.svg" width="400" alt="Godot Engine logo">
  </a>
</p>

## 2D and 3D cross-platform game engine

> **🔧 This is a fork with automation support!**
>
> This fork adds external automation capabilities to Godot's RemoteDebugger for use with [PlayGodot](https://github.com/Randroids-Dojo/PlayGodot). See the [Automation Branch](#automation-branch) section below.

**[Godot Engine](https://godotengine.org) is a feature-packed, cross-platform
game engine to create 2D and 3D games from a unified interface.** It provides a
comprehensive set of [common tools](https://godotengine.org/features), so that
users can focus on making games without having to reinvent the wheel. Games can
be exported with one click to a number of platforms, including the major desktop
platforms (Linux, macOS, Windows), mobile platforms (Android, iOS), as well as
Web-based platforms and [consoles](https://docs.godotengine.org/en/latest/tutorials/platform/consoles.html).

## Free, open source and community-driven

Godot is completely free and open source under the very permissive [MIT license](https://godotengine.org/license).
No strings attached, no royalties, nothing. The users' games are theirs, down
to the last line of engine code. Godot's development is fully independent and
community-driven, empowering users to help shape their engine to match their
expectations. It is supported by the [Godot Foundation](https://godot.foundation/)
not-for-profit.

Before being open sourced in [February 2014](https://github.com/godotengine/godot/commit/0b806ee0fc9097fa7bda7ac0109191c9c5e0a1ac),
Godot had been developed by [Juan Linietsky](https://github.com/reduz) and
[Ariel Manzur](https://github.com/punto-) (both still maintaining the project)
for several years as an in-house engine, used to publish several work-for-hire
titles.

![Screenshot of a 3D scene in the Godot Engine editor](https://raw.githubusercontent.com/godotengine/godot-design/master/screenshots/editor_tps_demo_1920x1080.jpg)

## Getting the engine

### Binary downloads

Official binaries for the Godot editor and the export templates can be found
[on the Godot website](https://godotengine.org/download).

### Compiling from source

[See the official docs](https://docs.godotengine.org/en/latest/engine_details/development/compiling)
for compilation instructions for every supported platform.

## Community and contributing

Godot is not only an engine but an ever-growing community of users and engine
developers. The main community channels are listed [on the homepage](https://godotengine.org/community).

The best way to get in touch with the core engine developers is to join the
[Godot Contributors Chat](https://chat.godotengine.org).

To get started contributing to the project, see the [contributing guide](CONTRIBUTING.md).
This document also includes guidelines for reporting bugs.

## Documentation and demos

The official documentation is hosted on [Read the Docs](https://docs.godotengine.org).
It is maintained by the Godot community in its own [GitHub repository](https://github.com/godotengine/godot-docs).

The [class reference](https://docs.godotengine.org/en/latest/classes/)
is also accessible from the Godot editor.

We also maintain official demos in their own [GitHub repository](https://github.com/godotengine/godot-demo-projects)
as well as a list of [awesome Godot community resources](https://github.com/godotengine/awesome-godot).

There are also a number of other
[learning resources](https://docs.godotengine.org/en/latest/community/tutorials.html)
provided by the community, such as text and video tutorials, demos, etc.
Consult the [community channels](https://godotengine.org/community)
for more information.

[![Code Triagers Badge](https://www.codetriage.com/godotengine/godot/badges/users.svg)](https://www.codetriage.com/godotengine/godot)
[![Translate on Weblate](https://hosted.weblate.org/widgets/godot-engine/-/godot/svg-badge.svg)](https://hosted.weblate.org/engage/godot-engine/?utm_source=widget)
[![TODOs](https://badgen.net/https/api.tickgit.com/badgen/github.com/godotengine/godot)](https://www.tickgit.com/browse?repo=github.com/godotengine/godot)

---

## Automation Branch

This fork's `automation` branch extends Godot's `RemoteDebugger` with external automation capabilities for testing and controlling games from external scripts.

### Features

The automation protocol adds the following commands to `core/debugger/remote_debugger.cpp`:

**Node Interaction**
- `get_node` - Get node info and properties by path
- `get_property` - Get a single property value
- `set_property` - Set a property value
- `call_method` - Call any method on any node

**Scene Management**
- `scene_tree` - Get the full scene tree structure
- `query_nodes` - Find nodes matching a pattern
- `count_nodes` - Count nodes matching a pattern
- `current_scene` - Get current scene path and name
- `change_scene` - Load a different scene
- `reload_scene` - Reload the current scene

**Game Control**
- `pause` - Get or set the pause state
- `time_scale` - Get or set the time scale
- `screenshot` - Capture a PNG screenshot

**Input Injection**
- `inject_mouse_button` - Simulate mouse clicks
- `inject_mouse_motion` - Simulate mouse movement
- `inject_key` - Simulate keyboard input
- `inject_action` - Simulate input actions
- `inject_touch` - Simulate touch input

### Building

```bash
# Clone this fork
git clone https://github.com/Randroids-Dojo/godot.git
cd godot
git checkout automation

# Build for macOS (Apple Silicon)
scons platform=macos arch=arm64 target=editor -j8

# Build for macOS (Intel)
scons platform=macos arch=x86_64 target=editor -j8

# Build for Linux
scons platform=linuxbsd target=editor -j8

# Build for Windows
scons platform=windows target=editor -j8
```

### Usage with PlayGodot

Use this build with [PlayGodot](https://github.com/Randroids-Dojo/PlayGodot) for Python-based game testing:

```python
from playgodot import Godot

async with Godot.launch("./my-game") as game:
    # Get node info
    node = await game.get_node("/root/Game")

    # Call methods
    result = await game.call_method("/root/Game", "get_score")

    # Simulate input
    await game.click(400, 300)
    await game.press_key(KEY_SPACE)

    # Take screenshots
    await game.screenshot("test.png")
```

### Modified Files

- `core/debugger/remote_debugger.cpp` - Automation command handlers
- `core/debugger/remote_debugger.h` - Automation method declarations

### Related Projects

- [PlayGodot](https://github.com/Randroids-Dojo/PlayGodot) - Python client library
- [Godot-Claude-Skills](https://github.com/Randroids-Dojo/Godot-Claude-Skills) - Claude Code skill for Godot development
