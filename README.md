# HomeSight Core

HomeSight Core turns RTSP camera streams into video segments that your application can store, inspect, and process. It provides a C++20 library with Python bindings for recording video, detecting motion, sampling frames, and storing recordings locally or in S3-compatible storage.

Use it as the video capture layer in a camera or surveillance application. It handles segment creation, stream reconnection, and recovery of pending work so you can build recording catalogs, event handling, and further video analysis on top.

## Record a camera

After [installing the library](#installation), replace the example RTSP URL with your camera's address and run this Python code:

```python
import time
from pathlib import Path

import homesight_core as core

config = core.SegmentRecorderConfig(
    camera=core.CameraStreamConfig(
        camera_name="front_door",
        rtsp_url="rtsp://user:password@192.168.1.50:554/stream1",
    ),
    storage=core.LocalDirectoryStorageConfig(root_directory=Path("recordings")),
    staging_directory=Path("staging"),
)

with core.SegmentRecorder(config) as recorder:
    time.sleep(20)

print(f"Saved {recorder.get_recorder_statistics().segments_stored} video segments.")
```

The example records for 20 seconds and stops when the context closes. Completed stored clips appear under `recordings/front_door/`. Keep the staging directory between runs so unfinished work can resume. For continuous recording and application notifications, see [recording events](docs/user-guide.md#recording-events).

## Installation

Use Python 3.12 or newer and `uv`. Before installing, follow the [native dependency instructions for Ubuntu or Fedora](docs/user-guide.md#installation). The library requires a C++20 compiler, CMake 3.22+, GStreamer 1.20+ with the camera's codec plugins, OpenCV, SQLite, and libcurl for S3 support.

From the repository root:

```sh
uv sync
```

The build uses your system's native libraries. For a C++ application, follow [the CMake installation and linking instructions](docs/user-guide.md#using-the-c-library).

## What you can do

- Record H.264 and H.265 camera video as MP4 or Matroska segments.
- Analyze motion across consecutive recordings and retain clips based on valid motion results.
- Sample RGB frames as NumPy arrays for further processing.
- Store recordings locally or upload them to S3-compatible storage with retry support.
- Limit local video storage and recover pending work after a restart.
- Consume durable recording and storage notifications in your application.

Recording captures video only. Motion analysis has a warm-up period, during which results can be unknown; [the user guide](docs/user-guide.md#motion-and-frame-sampling) explains how to handle those results.

## Documentation

Generate the unified Sphinx site and Doxygen C++ reference with the [documentation build instructions](docs/development.md#building-the-documentation).

- [User guide](docs/user-guide.md): installation, camera connections, recording, recovery, motion, sampling, S3, and C++ integration.
- [Development guide](docs/development.md): tests, datasets, packaging, sanitizers, and performance measurements.
- [Verification progress](IMPLEMENTATION_PROGRESS.md): completed checks and outstanding validation.

## Appendix: Open-source libraries and tools

HomeSight Core relies on these open-source projects for its runtime, builds, development, documentation, and optional integrations.

### Runtime libraries

- [GStreamer](https://gstreamer.freedesktop.org/): RTSP capture, video segmentation, decoding, and media inspection.
- [GLib](https://docs.gtk.org/glib/)/[GObject](https://docs.gtk.org/gobject/): Utilities, object properties, and signal callbacks used by the media pipelines.
- [OpenCV](https://opencv.org/): MOG2 background subtraction, motion masks, and frame resizing.
- [SQLite](https://sqlite.org/): Durable recording jobs, recovery state, and event journaling.
- [libcurl](https://curl.se/libcurl/): HTTP transfers and AWS Signature V4 authentication when S3 storage support is enabled.
- [NumPy](https://numpy.org/): RGB frame arrays exposed by the Python API.

### Bindings and build tools

- [pybind11](https://pybind11.readthedocs.io/en/stable/): Python bindings for the C++ library, including NumPy frame conversion.
- [scikit-build-core](https://scikit-build-core.readthedocs.io/en/latest/): CMake-backed Python package and wheel builds.
- [CMake](https://cmake.org/): Native build configuration, installation, and exported C++ package targets.
- [Ninja](https://ninja-build.org/): Build executor selected by the native CMake presets.
- [pkg-config](https://wiki.freedesktop.org/Software/pkg-config/)/[pkgconf](https://github.com/pkgconf/pkgconf): Discovery of GStreamer compiler and linker flags through CMake.
- [uv](https://docs.astral.sh/uv/): Python environment setup, dependency management, command execution, and package builds.

### Development, testing, and notebooks

- [pytest](https://docs.pytest.org/en/stable/): Python, native-helper, packaging, and integration test orchestration.
- [Ruff](https://docs.astral.sh/ruff/): Python linting and formatting.
- [mypy](https://mypy-lang.org/): Static type checking for the Python package and tests.
- [Pydantic](https://docs.pydantic.dev/latest/): Validation of test settings and media metadata.
- [pydantic-settings](https://docs.pydantic.dev/latest/concepts/pydantic_settings/): Integration-test configuration from environment variables and `.env` files.
- [Rich](https://rich.readthedocs.io/en/stable/): Live segment tables and terminal reports during camera and S3 tests.
- [Jupyter](https://jupyter.org/): Notebook tools included in the development environment for interactive exploration.
- [IPython](https://ipython.org/): Interactive Python shell included in the development environment.
- [FFmpeg/ffprobe](https://ffmpeg.org/): Generated video fixtures, reference frame decoding, and playback and metadata verification.
- [PyGObject](https://pygobject.gnome.org/): Python access to GLib and GStreamer in the optional RTSP replay tests.
- [GStreamer RTSP Server](https://gstreamer.freedesktop.org/documentation/gst-rtsp-server/): Loopback replay of camera footage for optional integration tests.

### Documentation

- [Sphinx](https://www.sphinx-doc.org/): Unified guides and Python/C++ API documentation.
- [Alabaster](https://alabaster.readthedocs.io/en/latest/): HTML theme for the Sphinx site.
- [Breathe](https://breathe.readthedocs.io/en/latest/): Integration of Doxygen's C++ XML output into Sphinx.
- [MyST Parser](https://myst-parser.readthedocs.io/en/latest/): Markdown parsing for the Sphinx guides.
- [sphinxcontrib-mermaid](https://sphinxcontrib-mermaid-demo.readthedocs.io/en/latest/): Mermaid diagram support in Sphinx.
- [Mermaid](https://mermaid.js.org/): Architecture and application-flow diagrams rendered in the browser.
- [Doxygen](https://www.doxygen.nl/): C++ reference generation from public headers, including XML consumed by Breathe.

### Optional integration services

These components support optional local storage and integration-test workflows.

- [Docker Engine](https://docs.docker.com/engine/) and [Docker Compose](https://docs.docker.com/compose/): Isolated storage and platform-test containers, plus the local S3 service stack.
- [PGSTY Silo](https://github.com/pgsty/silo): PGSTY-maintained fork of MinIO Server, configured as the S3-compatible test server with the `pgsty/silo` image.
- [PGSTY mc](https://github.com/pgsty/mc): PGSTY-maintained fork of MinIO Client, configured with the `pgsty/mc` image to create the Compose test bucket.
