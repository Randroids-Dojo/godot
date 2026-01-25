# Agent Guidelines for Godot Automation Fork

Guidelines to avoid common mistakes when working on this codebase.

## C++ Include Requirements

**Always explicitly include headers for any types or functions you use.** Do not rely on transitive includes - they vary by platform and can cause CI failures on some builds but not others.

Common includes that must be explicit:

| Function/Type | Required Include |
|---------------|------------------|
| `callable_mp_static` | `#include "core/object/callable_method_pointer.h"` |
| `callable_mp` | `#include "core/object/callable_method_pointer.h"` |
| `CONNECT_ONE_SHOT` | `#include "core/object/object.h"` |
| `Ref<T>` | `#include "core/object/ref_counted.h"` |
| `Vector`, `List`, `HashMap` | `#include "core/templates/<container>.h"` |

## Automation Protocol (remote_debugger.cpp)

When modifying automation commands in `core/debugger/remote_debugger.cpp`:

1. **Add `is_inside_tree()` guards** - Always check if the scene tree is initialized before accessing nodes:
   ```cpp
   Node *root = tree->get_root();
   ERR_FAIL_NULL(root);
   if (!root->is_inside_tree()) {
       // Send empty/error response
       return;
   }
   ```

2. **Scene changes are deferred** - `change_scene_to_file()` returns immediately but the scene isn't loaded yet. Connect to `scene_changed` signal if you need to respond after the scene is ready.

3. **Flush input events** - After injecting input events, call `DisplayServer::get_singleton()->process_events()` to ensure they reach the GUI system in headless mode.

## Testing Changes

The CI builds for multiple platforms (Linux, macOS, Windows). A local build passing does not guarantee CI will pass due to:
- Different include resolution
- Platform-specific code paths
- Compiler differences (GCC vs Clang vs MSVC)
