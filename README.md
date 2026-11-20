# CCXVis

**Results visualization for CalculiX.**

CCXVis is a Linux desktop post-processor for saved CalculiX CrunchiX results. Use it to inspect nodal contours and deformed meshes, cut sections through a model, animate saved result frames, and plot time histories or variable-against-variable curves. It uses a Qt Widgets interface and a VTK viewport.

| Task | Menu |
| --- | --- |
| Open an FRD results file | **File → Open FRD** |
| Choose a frame, result field, or component | **Result → Step/Frame** and **Result → Field Output** |
| Inspect a cross-section or read a nodal value | **Tools → Section View** or **Tools → Probe** |
| Preview a contour sequence or export an MP4 | **Animate → Contour Animation** |
| Plot CSV, whitespace tables, or CalculiX nodal DAT output | **Tools → XY Plot** |
| Save the model view and its overlays as a PNG | **File → Export PNG** |

## Build and run

Use a C++17 compiler, CMake 3.20 or newer, and Ninja for the commands below. The desktop build needs Qt Widgets and Concurrent, plus VTK built against the same Qt major version. The Qt 6 build also uses Qt OpenGL Widgets. A working OpenGL desktop session is required for the 3D viewport and image or movie capture.

### Standalone Qt 6 and VTK

For a standalone Qt 6 installation, build VTK 9.5.2 with `VTK_QT_VERSION=6` and `VTK_MODULE_ENABLE_VTK_GUISupportQt=YES`. Pass the Qt and VTK installation prefixes to CMake when they are outside the system search path. [VTK build settings](https://docs.vtk.org/en/v9.5.2/build_instructions/build_settings.html)

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/path/to/qt6;/path/to/vtk"
cmake --build build
./build/ccxvis /path/to/results.frd
```

### Rocky Linux 9 with ParaView RPMs

Use the Qt 5 development packages and the VTK headers and libraries from the `paraview-openmpi-devel` RPM for the Qt 5 and OpenMPI build of ParaView.

```sh
cmake -S . -B build-rocky -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCCXVIS_QT_MAJOR=5 -DCCXVIS_PARAVIEW_VTK_ROOT=/usr
cmake --build build-rocky
./build-rocky/ccxvis /path/to/results.frd
```

Launch `ccxvis` without a path to start with an empty viewer. To stage a local installation, run `cmake --install build --prefix "$PWD/build/install"`. For the ParaView RPM configuration, substitute `build-rocky` for both `build` paths.

## Inspect results

1. Choose **File → Open FRD** or pass the FRD path on launch.
2. Choose **Result → Step/Frame** and **Result → Field Output** to select a result and component. The toolbar also provides a **Frame** selector and a **Field** button. The viewer offers displacement magnitude and von Mises stress when the required components are available.
3. Drag to orbit, use the wheel to zoom, or select a standard view in **View**. **View → Fit** centers the mesh. **View → Orthogonal** switches between orthographic and perspective projection.
4. Turn on **Plot → Show deformation** when the frame includes complete displacement data. An **Options → Deformation scale** value of 1 applies the recorded displacements directly. Use global displacement output (`GLOBAL=YES`) in CalculiX so the vectors align with the mesh coordinates.
5. Choose a palette under **Options → Colormap**. Under **Options → Color scale**, select **Discrete** and set **Steps** to divide the result range into equal-width color bands on the model and legend.
6. Choose **Tools → Probe** and click a visible node to see its original node ID, reference coordinates, and current value in the status bar. **File → Export PNG** saves the viewport with its legend.

Right-click the viewport and choose **Set Rotation Center** to orbit around the clicked point. A click on the model uses the visible surface or wireframe. A click on the background keeps the current focal depth. **View → Fit** restores the model center.

The **Plot** menu offers these display modes.

| Mode | Display |
| --- | --- |
| Surface | Shaded geometry and result contours |
| Edge | Wireframe of exterior element edges, colored by the selected result when available |
| Mesh | Shaded geometry with an exterior element-edge overlay |
| Featured Edge | Shaded geometry with open boundaries, non-manifold edges, and creases over 30° |
| Featured Edge with Mesh | Both overlays, with featured edges emphasized |

Featured edges are identified on the reference mesh. Both overlays follow the displayed deformation.

The viewer opens one read-only mesh at a time. It supports short and long ASCII FRD, little-endian CalculiX binary FRD, and real nodal results. Gray contour regions indicate missing nodal values. Values use the solver's unit system.

## Inspect a section

Choose **Tools → Section View** to open the controls below the viewport. Select X, Y, or Z, then move the slider or enter a plane coordinate in model units. **Flip Side** switches the half of the model that remains visible. Toggle **Section View** off to restore the full model.

Solid sections show the selected contour interpolated on the displayed geometry, including deformation. Shells and beams show intersection lines and points. **Edge** shows the clipped exterior wireframe. Right-click the cut to set the rotation center. **Probe** continues to report original visible mesh nodes, and **Export PNG** captures the current section and legend.

## Animate a contour

1. Open an FRD file and select the field and component to animate. Set the camera, palette, deformation scale, and section plane before opening **Animate → Contour Animation**.
2. Select **First frame**, **Last frame**, and **FPS**. The dialog lists compatible saved frames from the active analysis series in FRD order, including saved modes, and reports frames missing the selected field or component. The default range covers all matching frames at 12 FPS. Playback and export require at least two matching frames.
3. Use **Play** to preview, **Pause** to hold a frame, or **Stop** to return to the original view. Choose **Export MP4** to capture the selected sequence to a file.

CCXVis computes one finite color range for the whole selection so the same color represents the same value throughout. With deformation enabled, each selected frame needs complete, usable nodal displacements. The animation retains the camera, palette, color bands, section plane, legend, and orientation triad, and adds the step, increment, or mode label to the viewport.

Preview works independently of FFmpeg. MP4 export requires a working `ffmpeg` on `PATH` with the `libx264` or `libopenh264` encoder. It writes one H.264 video frame per matching FRD frame at the chosen FPS, using `yuv420p` and even image dimensions. You can cancel during capture or encoding. CCXVis publishes the destination after encoding succeeds. If export fails to start, check that `ffmpeg -version` runs from the environment used to launch CCXVis. [FFmpeg codec documentation](https://ffmpeg.org/ffmpeg-codecs.html)

## Plot an XY curve

Choose **Tools → XY Plot** and open a data file. Each file opens in its own plot window, including when the FRD viewport is empty. Choose the **X axis** and check one or more **Y series**. The colored keys identify the curves. Scroll to zoom, drag to pan, and use **Reset View** to return to the full data range.

| Input | Default X | How samples are paired |
| --- | --- | --- |
| Comma-separated CSV or a whitespace table (`.dat` or `.txt`) | First column | Values in the same row |
| Native CalculiX nodal `.dat` output | **Time** | A selected X series is matched to each Y series by exact recorded time |

Tables accept optional headers, quoted CSV fields, and `E` or `D` exponents. Blank and nonfinite samples create gaps, and malformed rows report their line number. Save the following as `history.csv` to try a time history or choose **Displacement (mm)** as X to plot a force–displacement curve.

```csv
Time (s),Displacement (mm),Force (N)
0.0,0.0,0.0
0.1,0.2,12.5
0.2,0.5,26.0
0.3,0.2,9.0
```

Native DAT input covers supported `*NODE PRINT` blocks for displacement, velocity, temperature, force, heat generation, pressure, and mass flow, including printed force and heat totals. Series labels identify the quantity, set, node or set total, and component. The `(L)` marker identifies values printed in local coordinates. Time-aligned XY curves keep the recorded Y sample order so loops stay intact. The plot reports unmatched times and ambiguous duplicate times. A DAT file containing only blocks such as `*EL PRINT` or frequency tables produces an unsupported-input message.

## Test

```sh
cmake -S . -B build-headless -G Ninja -DCCXVIS_BUILD_GUI=OFF
cmake --build build-headless
ctest --test-dir build-headless --output-on-failure
```

Run `ctest --test-dir build-rocky --output-on-failure` for a Rocky Linux desktop build. GUI builds add curve parsing and pairing, animation state and encoder coordination, and section geometry tests. These tests run without a working OpenGL desktop.

Set `CCXVIS_BASELINE_FRD` at configure time to the absolute path to CGX 2.23's `examples/result.frd` to add a modal parity test and, for GUI builds, an offscreen desktop workflow test. With the same fixture, set `CCXVIS_TEST_RENDERING=ON` and run CTest from a working graphical session to exercise viewport PNG capture.

## Source and license

The FRD block layouts and decoding flow in `src/FrdDocument.cpp` were adapted from Klaus Wittig's CalculiX GraphiX 2.23 `src/readfrd.c`. The upstream file SHA-256 is `641bb8256fb7b80417c7ad3576ea83465f3de2eb54995f87464755e950ac7db8`. The adaptation uses compact mesh indices and checked, deferred field reads. [CalculiX source and downloads](https://dhondt.de/)

This source is distributed under [GPL-2.0](LICENSE). Qt and VTK are external dependencies with their own license terms.
