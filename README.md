# Facet

A personal desktop 3D modeler for subdivision and high-poly work, with **Maya-style
interaction**: the same camera controls, hotkeys, marking menus, hotbox, component modes,
Channel Box and Outliner workflow. It is a standalone app written in C++20, Qt 6 and OpenGL 4.1.

![four-view layout](docs/quad.png)

## Build

Requirements: a C++20 compiler, CMake ≥ 3.21, Qt 6 (Widgets, OpenGL, OpenGLWidgets), OpenGL 4.1.

**macOS**

```sh
brew install qt cmake ninja
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build
open build/Facet.app
```

**Windows** (Visual Studio 2022 + Qt online installer, MSVC 64-bit kit)

```bat
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:\Qt\6.7.2\msvc2019_64
cmake --build build --config Release
C:\Qt\6.7.2\msvc2019_64\bin\windeployqt build\Release\Facet.exe
build\Release\Facet.exe
```

**Linux** (Ubuntu 24.04)

```sh
sudo apt install cmake ninja-build qt6-base-dev libgl-dev
cmake -S . -B build -G Ninja && cmake --build build
./build/Facet
```

Command-line options: `Facet scene.facet`, `Facet model.obj`, `Facet --demo [faces|soft]`,
`Facet --quad`, `Facet --screenshot out.png`.

### Tests

```sh
./build/facet_tests                 # geometry core: topology, subdivision, every modeling op
xvfb-run -a ./build/facet_uitests   # drives the real window with synthesized mouse/keys
```

The UI tests click, marquee, drag gizmos, extrude, insert loops, use marking-menu gestures and
check hotkeys against the real viewport and its GPU picking.

## What's in v0.1

**Modeling core (built for dense meshes)**
- Index-based half-edge mesh with n-gons, borders and non-manifold tolerance; parallel normals.
- Catmull–Clark subdivision with **semi-sharp creases** (Pixar rules: crease 1/2/3/full).
  Topology is refined once and cached, so dragging vertices only re-evaluates positions:
  a 100k-quad cage → 1.6M-quad smooth surface re-evaluates in ~10 ms.
- Ops: Extrude (together / individual), Insert Edge Loop (interactive with slide preview),
  Delete faces, dissolve edges and vertices, Merge by distance, Merge to Center, Collapse,
  Fill Hole, Mirror (welded seam), Combine, Separate, Smooth (bake), Triangulate,
  Reverse normals, Freeze / Reset transforms, Center pivot.
- Primitives with **construction history**: edit the INPUTS (subdivisions, radius…) in the
  Channel Box and the mesh regenerates, until you edit components (as in Maya).

**Maya-style interaction**

| Input | Action |
|---|---|
| Alt + LMB / MMB / RMB, wheel | Tumble / track / dolly |
| F, A (Shift = all views) | Frame selected / all |
| Space tap / hold | Four-view toggle (maximize panel under cursor) / Hotbox |
| Q W E R | Select / Move / Rotate / Scale |
| MMB-drag (no Alt) | Drive the last-used manipulator handle from anywhere |
| X / J held | Grid snap while moving / 15° rotate snap |
| F8, F9, F10, F11 | Object↔component, vertex, edge, face |
| Ctrl+F9/F10/F11 | Convert selection |
| Shift / Ctrl / Ctrl+Shift click | Toggle / deselect / add |
| Double-click edge | Select edge loop |
| > / < | Grow / shrink selection |
| B tap, B + LMB drag | Soft selection on/off, falloff radius (world-space, coloured falloff) |
| RMB | Marking menu: component modes |
| Shift + RMB | Marking menu: modeling tools for the current mode |
| Ctrl + RMB | Marking menu: convert selection |
| Ctrl+E | Extrude (manipulator switches to normal orientation) |
| G | Repeat last command |
| 1 / 2 / 3, PgUp / PgDn | Smooth preview off / cage + smooth / smooth, smooth level |
| 4 / 5 | Wireframe / shaded |
| Alt+X, Alt+B | X-ray, cycle background |
| Z, Shift+Z (and Ctrl+Z / Ctrl+Shift+Z) | Undo / redo |
| Ctrl+D, Delete, Ctrl+H / Shift+H | Duplicate, delete, hide / show all |

Marking menus work as gestures (press, flick, release) or as click menus (quick click).
Channel Box labels are virtual sliders: drag a label to scrub the value.
Selection uses an integer GPU ID buffer: marquee picks **only visible** components unless
X-ray or wireframe is on, and components under the cursor get a red pre-selection highlight.
The HUD shows cage counts plus the smooth-preview triangle count.

Platform notes: on macOS, Maya's Ctrl shortcuts are on ⌘, Alt is ⌥, and F8–F11 may need
`fn`. On Linux, if your window manager uses Alt+drag to move windows, rebind it to Super.

## Architecture

```
src/math      Vec/Mat/AABB/Ray (no dependencies)
src/mesh      Mesh (half-edge), Subdivision, MeshOps, Primitives, ObjIO   ← pure C++, unit-tested
src/scene     Scene, objects + construction history, undo commands, Modeling (high-level commands)
src/render    OpenGL 4.1 core renderer: studio-lit shading, cage/smooth wires, overlays, ID pick pass
src/viewport  Viewport widget: cameras, picking, manipulators, tools, HUD
src/ui        MainWindow (menus/shelf/status line), MarkingMenu + Hotbox, Outliner, ChannelBox
```

The geometry core has no Qt dependency, so it can later be driven by a Python layer or a
headless batch tool. GPU and subdivision caches are keyed on globally unique mesh version ids,
so undo/redo just swaps mesh snapshots and everything stays in sync.

## Roadmap (high-poly / VFX priorities)

1. **Bevel / chamfer** with segments and profile (the key hard-surface support-loop tool), Bridge, Multi-Cut, Offset Edge Loop, Target Weld, Slide Edge.
2. **Symmetry modeling** (live mirrored component edits across a world or object axis).
3. **UVs**: face-varying UVs preserved through all ops, OBJ UV I/O, unfold, UV editor.
4. **Sculpting**: multires / dynamic-tessellation brushes (grab, smooth, crease, inflate, clay) on millions of polys, with masks and layers.
5. **Retopology**: Quad Draw-style tool snapping to a high-poly live surface.
6. Hard/soft edge normals, normal locking; displacement and normal baking.
7. Python scripting (`cmds`-like) and a Script Editor; customizable hotkeys and shelves.
8. Interchange: FBX, USD and Alembic; GPU subdivision via OpenSubdiv for very dense previews.
