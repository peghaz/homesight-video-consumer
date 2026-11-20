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
