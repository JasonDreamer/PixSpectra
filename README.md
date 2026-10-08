# PixSpectra

An After Effects effect that encodes image channels as audio-like data, processes them through a VST3 effect, and decodes the result back into an image.

## Highlights

- RGB-column IFFT to dual-mono PCM, VST3 processing, and FFT-based RGB reconstruction.
- Ships as PixSpectra.aex plus the in-process PixSpectraEngine.dll.
- The original alpha channel is preserved through the encode/decode path.

## Build environment

- Windows x64 and Adobe After Effects Effect SDK 2025 (obtain the SDK separately).
- Visual Studio 2022 C++ v143, Windows SDK, and CUDA Toolkit 13.0 for CUDA-enabled projects.
- Supported pixel formats: 8/16bpc; 32bpc and Smart Render are not supported.

Host compatibility can vary by After Effects version. Refer to a release's validation notes for the versions and render paths verified.

## Build from source

Keep this project under `MyEffects/<Effect>` in the Adobe After Effects SDK 2025 directory; the solution uses paths relative to that workspace. Set `AE_PLUGIN_BUILD_DIR` to a writable output directory, open the solution in `Win/` with Visual Studio, and build `Release|x64`.

Adobe SDK files and generated binaries are not included in the source repository.

## Third-party notices

See [THIRD_PARTY.md](THIRD_PARTY.md) for the pinned JUCE dependency, licensing, and source setup.

## Installation

Download the `.aex` package from GitHub Releases, place it in an After Effects plug-ins folder, and restart After Effects. Install PixSpectraEngine.dll beside the AEX file.

## Releases

[GitHub Releases](https://github.com/JasonDreamer/PixSpectra/releases)

Published releases include the effect binary and engine dependency, matching source revision, build environment, supported After Effects versions, and validation results.

## License

No license is granted for this source code at this time. Do not reuse or redistribute it without the copyright holder's permission. The Adobe After Effects SDK and CUDA Toolkit are separate products and are not included.
