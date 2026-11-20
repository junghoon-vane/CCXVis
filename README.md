# CCXVis

CCXVis is a Linux desktop application for visualizing CalculiX results. It adapts the CalculiX GraphiX 2.23 FRD reader for use with a Qt Widgets interface and a VTK viewport.

## Run on Rocky Linux 9

The desktop build requires a C++17 compiler, CMake 3.20 or newer, Qt Widgets, and VTK with Qt support. An OpenGL-capable desktop session is required for 3D rendering.

For a standalone Qt 6 and VTK installation, build VTK 9.5.2 with `VTK_QT_VERSION=6` and `VTK_MODULE_ENABLE_VTK_GUISupportQt=YES`. Pass the Qt and VTK installation prefixes to CMake if they are outside the system search path.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="/path/to/qt6;/path/to/vtk"
cmake --build build
./build/ccxvis /path/to/results.frd
```

On Rocky Linux 9 with the Qt 5 and OpenMPI builds of ParaView already installed, use the Qt 5 development headers and the VTK headers and libraries from the `paraview-openmpi-devel` RPM.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCCXVIS_QT_MAJOR=5 -DCCXVIS_PARAVIEW_VTK_ROOT=/usr
cmake --build build
./build/ccxvis /path/to/results.frd
```

Use `cmake --install build --prefix "$PWD/build/install"` to stage a local installation.

## Inspect results

1. Choose **File → Open FRD** or pass the FRD path on launch.
2. Choose **Result → Step/Frame** and **Result → Field Output** to select a result and component. The toolbar also provides a **Frame** selector and a **Field** button. The viewer offers displacement magnitude and von Mises stress when the required components are available.
3. Drag to orbit, use the wheel to zoom, or select a standard view in **View**. **View → Fit** centers the mesh. **View → Orthogonal** switches between orthographic and perspective projection.
4. Turn on **Plot → Show deformation** when the frame includes complete displacement data. An **Options → Deformation scale** value of 1 applies the recorded displacements directly. Use CCX’s default global coordinate output for deformation. FRD does not identify local displacement bases when CCX writes them with `GLOBAL=NO`.
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

The viewer opens one read-only mesh at a time. It supports short and long ASCII FRD, little-endian CalculiX binary FRD, and real nodal results. Gray contour regions indicate missing nodal values. Values use the solver's unit system. Material-dependent and element result contours, complex reconstruction, animation, and CGX scripting are outside this release.

## Inspect a section

Choose **Tools → Section View** to open the controls below the viewport. Select X, Y, or Z, then move the slider or enter a plane coordinate in model units. **Flip Side** switches the half of the model that remains visible. Toggle **Section View** off to restore the full model.

Solid sections show the selected contour interpolated on the displayed geometry, including deformation. Shells and beams show intersection lines and points. **Edge** shows the clipped exterior wireframe. Right-click the cut to set the rotation center. **Probe** continues to report original visible mesh nodes, and **Export PNG** captures the current section and legend.

## Test

```sh
cmake -S . -B build-headless -G Ninja -DCCXVIS_BUILD_GUI=OFF
cmake --build build-headless
ctest --test-dir build-headless --output-on-failure
```

Set `CCXVIS_BASELINE_FRD` to the absolute path to CGX 2.23's `examples/result.frd` to add the modal parity test. GUI builds include an offscreen section geometry test. With the CGX fixture they also run the offscreen desktop workflow test. Set `CCXVIS_TEST_RENDERING=ON` and run CTest from a working graphical session to verify the viewport PNG.

## Source and license

The FRD block layouts and decoding flow in `src/FrdDocument.cpp` were adapted from Klaus Wittig's CalculiX GraphiX 2.23 `src/readfrd.c`. The upstream file SHA-256 is `641bb8256fb7b80417c7ad3576ea83465f3de2eb54995f87464755e950ac7db8`. The adaptation uses compact mesh indices and checked, deferred field reads. The original CGX source and command-line tool remain available independently.

This source is distributed under [GPL-2.0](LICENSE). Qt and VTK are external dependencies with their own license terms.
