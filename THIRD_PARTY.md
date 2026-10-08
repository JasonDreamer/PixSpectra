# Third-party dependencies

## JUCE

- Version: 8.0.12 (`29396c22c93392d6738e021b83196283d6e4d850`).
- Source: [JUCE 8.0.12](https://github.com/juce-framework/JUCE/tree/8.0.12).
- License: The [JUCE 8.0.12 license terms](https://github.com/juce-framework/JUCE/blob/8.0.12/LICENSE.md) describe the AGPLv3 and commercial JUCE licensing options. Review and comply with terms applicable to your use before building or redistributing a product that includes JUCE. This repository does not redistribute JUCE source or grant a JUCE license.
- VST3 hosting is provided by JUCE's hosting implementation and SDK headers.
- `Host/SoftwareUI.cmake` applies two checked, repeatable build-time patches to the pinned source: Windows peers use GDI and native images use `SoftwarePixelData` when `PIXSPECTRA_SOFTWARE_UI=1`. This setting is scoped to PixSpectraEngine; JUCE build tools retain upstream behavior. Configuration fails if the source does not match the expected version.

To obtain the pinned JUCE source, clone it into `third_party/JUCE` from the project root:

```powershell
git clone --depth 1 --branch 8.0.12 https://github.com/juce-framework/JUCE.git third_party/JUCE
```

Adobe After Effects SDK and CUDA Toolkit are separate build dependencies and are not included in this repository.
