# Build dependencies

- JUCE 8.0.12, commit `29396c22c93392d6738e021b83196283d6e4d850`.
- Source: https://github.com/juce-framework/JUCE/tree/8.0.12
- Local path: `third_party/JUCE` (excluded from Git). `Host/SoftwareUI.cmake` applies two checked, repeatable patches to the pinned source: default Windows peers use GDI and default native images use SoftwarePixelData when `PIXSPECTRA_SOFTWARE_UI=1`. The definition is scoped to PixSpectraEngine; JUCE build tools retain upstream behavior. A source mismatch fails configuration.
- License text: `third_party/JUCE/LICENSE.md` and module source headers. JUCE offers AGPLv3 and commercial licensing; no distribution license selection is implied by this local implementation.
- VST3 hosting is provided by JUCE's included implementation and SDK headers.
- CUDA 13.0 and the Adobe AE SDK come from this workspace's existing toolchain.

To restore the vendor dependency on another checkout:

```powershell
git clone --depth 1 --branch 8.0.12 https://github.com/juce-framework/JUCE.git third_party/JUCE
```

Run the workspace's `tools/Build-MknbEffect.ps1 -Effect PixSpectra` to build both the AEX and engine DLL. Research-only Python wheels are not product dependencies.
