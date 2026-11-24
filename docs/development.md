# Development guide

This guide covers building, testing, and verifying HomeSight Core. For application setup, recording, storage, and C++ integration, see the [user guide](user-guide.md). Detailed verification history and unresolved findings remain in {download}`IMPLEMENTATION_PROGRESS.md <../IMPLEMENTATION_PROGRESS.md>`.

Run all commands from the repository root. Install the [native dependencies](user-guide.md#installation) first, then prepare the Python environment and run the default checks:

```sh
uv sync
uv run pytest
uv run ruff check src/homesight_core tests
uv run ruff format --check src/homesight_core tests
uv run mypy
```

Pytest is the single test runner, including for native helpers, installation, and integration checks. The `asan` and `tsan` CMake presets build separate sanitizer variants. Integration suites are opt-in and use the inputs described below. Playback verification also requires `ffprobe` from FFmpeg.

Frame-sampling unit tests generate short, deterministic lossless clips with `ffmpeg` to check interval boundaries, sparse frames, and variable presentation times. Install FFmpeg to run these tests; they skip explicitly when it is unavailable. These generated fixtures are separate from the actual-camera footage used by the opt-in media suites.

Build the Python wheel and source distribution with:

```sh
uv build
```

## Verification inputs

Copy {download}`.env.example <../.env.example>` to `.env` and fill only the suites you need. Media suites use actual camera recordings; replay derivatives must retain their provenance. S3 suites use real S3-compatible servers, including the isolated MinIO instances owned by the resilience and measurement fixtures. The default suite checks configuration, ownership, journal recovery, event delivery, quota, and worker lifetimes without a camera. Selected integration suites fail clearly when their inputs are missing.

Run **Real RTSP Test with S3** to record directly from your camera, upload through the native recorder, download the stored segments, and verify playback and metadata:

```sh
uv run pytest -m real_rtsp_s3 -v
```

Set `TEST_S3_ENDPOINT`, `TEST_S3_BUCKET`, `TEST_S3_ACCESS_KEY`, and `TEST_S3_SECRET_KEY` in `.env`; set `TEST_S3_SESSION_TOKEN` when using temporary credentials. You can also set `TEST_RTSP_URL` in `.env`, or supply the camera URL directly from your terminal:

```sh
TEST_RTSP_URL='rtsp://user:password@192.168.1.50:554/stream1' \
TEST_CAMERA_TEST_SECONDS=60 \
uv run pytest -m real_rtsp_s3 -v
```

Replace the example URL with your camera's RTSP URL. These inline environment variables override `.env` for this command only. This example records for 60 seconds; the default is 25 seconds when `TEST_CAMERA_TEST_SECONDS` is unset in both the environment and `.env`. This test does not require `TEST_SEGMENT`.

The command displays a live Rich table without requiring `-s`, then leaves the final table visible. Each reported completed segment has one row showing its filename, duration, analysis status, motion result, peak and mean changed-area percentages, changed/analyzed frame counts, and S3 status. Rows update as uploads and verification finish. Confirmed motion produces one console notice per segment. Elapsed time and recorder/upload counts appear below the table; redirected output receives a static final table.

`Motion` is `Yes` or `No` only for a valid analysis result. Warm-up, insufficient data, failed analysis, and missing results show `Unknown`; unavailable or unreliable metrics show `—`. The model warms up for 30 seconds by default, so early segments can be unknown even when there is visible activity. Available recorder, motion, and storage errors appear below the table.

The test uploads segments under a unique S3 prefix, downloads them, and verifies playback and metadata. **By default it deletes its uploaded objects afterward**, including on assertion failures. To keep videos for inspection in the bucket, run:

```sh
uv run pytest -m real_rtsp_s3 -v --keep-s3-objects
```

`--keep-s3-objects` applies to both S3 integration tests and keeps their uploads even when verification fails. The output prints the destination prefix and uploaded S3 URIs, with `Kept`, `Deleted`, or `Cleanup failed` showing the actual outcome. Retained videos remain for you to delete manually. S3 support must be enabled in the compiled package; it is enabled by default. Pytest configuration and marker descriptions live in {download}`pytest.ini <../pytest.ini>`.

```sh
uv run pytest --collect-only -q
uv run pytest tests/test_s3.py::test_real_rtsp_with_s3 --live --s3 -v
uv run pytest --live
uv run pytest --dataset
uv run pytest --s3
uv run pytest --packaging
uv run pytest --resilience
uv run pytest --media
uv run pytest --measurements
uv run pytest --platforms
uv run pytest tests/test_recovery.py --native-preset asan
uv run pytest tests/test_recovery.py --native-preset tsan
uv run ruff check src/homesight_core tests
uv run ruff format --check src/homesight_core tests
uv run mypy
```

`TEST_RTSP_URL` selects an actual camera. `TEST_SEGMENT` selects a short actual camera recording for sampling and S3 checks. `TEST_DATASET_MANIFEST` points to JSON with chronological recordings and independently assigned labels:

```json
{"reviewed_by": "camera-owner", "reviewed_at": "2026-11-20T08:00:00Z", "recordings": [
  {"path": "first.mp4", "sha256": "<actual SHA-256>", "continuity_id": "session-1", "start_media_seconds": 0, "expected_status": "warming_up", "has_motion": null},
  {"path": "after-warmup.mp4", "sha256": "<actual SHA-256>", "continuity_id": "session-1", "start_media_seconds": 40, "expected_status": "valid", "has_motion": true},
  {"path": "still.mp4", "sha256": "<actual SHA-256>", "continuity_id": "session-1", "start_media_seconds": 50, "expected_status": "valid", "has_motion": false}
]}
```

Include every intervening recording in the real manifest; gaps reset the model. Paths resolve relative to the manifest. Replace the digest placeholders with each file's actual SHA-256 and record the human reviewer's name and timezone-aware review time. Only `valid` entries may have boolean motion labels. The dataset must contain warm-up, valid motion and valid stillness. The tests verify the file hashes before analysis, compare the chronological results with the confirmed labels, and run actual deletion through the native recorder. Only a valid negative result may delete footage.

Prepare labels independently of analyzer output. Copy retained files into an ignored review directory, group them by their original recording session/continuity ID, and order them by journal ordinal or durable event sequence, never by UUID filename or filesystem modification time. Preserve media start times. Provide a numbered playback list, play every intervening clip, and obtain human confirmation of motion/stillness before creating the runnable manifest. Mark uncertain clips explicitly in the review inventory. If the reviewed set lacks either class after warm-up, retain another chronological camera session and review it before running `--dataset`. An unlabelled review inventory is not a test manifest.

S3 tests require a real configured endpoint, bucket and credentials, create an isolated random prefix, and delete their test objects unless `--keep-s3-objects` is selected. `--gpu` exercises hardware-enabled decoder selection but does not establish that a hardware decoder was chosen. GPU acceleration and extended production soak testing remain optional and unverified.

### Isolated failure recovery and media coverage

```sh
# Cache the real server image once; tests use --pull=never.
docker pull pgsty/silo:RELEASE.2026-11-12T00-00-00Z
TEST_REPLAY_SEGMENT=/absolute/path/to/camera-session.mkv uv run pytest --resilience -v
TEST_H264_SEGMENT=/absolute/path/to/h264.mp4 \
TEST_H265_SEGMENT=/absolute/path/to/h265.mkv uv run pytest --media -v
```

`--resilience` requires Docker access, `curl` with SigV4 support, FFmpeg's `ffprobe`, and a system Python with PyGObject plus the GStreamer RTSP server typelib. On Ubuntu install `python3-gi gir1.2-gst-rtsp-server-1.0`; on Fedora install `python3-gobject gstreamer1-rtsp-server`. Set `TEST_RTSP_PYTHON` if that interpreter is not `/usr/bin/python3`. The replay input must be actual H.264/H.265 footage at least 30 seconds long. The media matrix requires a recording of each codec at least 12 seconds long; it writes both MP4 and Matroska, verifies playback and timestamps, and checks conservative resets. Record when footage was remuxed or transcoded; a replay derivative does not establish coverage of a camera's native encoder.

Each resilience test owns a loopback RTSP subprocess and, when needed, a uniquely named `homesight-pytest-*` MinIO container with disposable credentials, temporary storage and a dedicated loopback port. Fixtures terminate only those processes and remove only those containers. They do not control the actual camera, the Compose stack, existing buckets or retained S3 objects. The suite interrupts/restarts the replay server, stops/restarts its own MinIO, verifies upload retry and pending-video eviction, and kills its own recorder subprocess to check journal/event recovery. Diagnostics remain in pytest's temporary directory; use a unique `--basetemp=build/verification/<run-name>` to retain them.

### Build compatibility and sanitizers

`uv run pytest --packaging -v` builds and inspects the wheel/sdist, installs wheels in clean environments outside the checkout, and rebuilds the extracted sdist there. It also builds separate Debug, Release and S3-disabled native installations. External consumers compile every installed public header first in its own translation unit, link only `HomesightCore::homesight_core`, and run without private source include paths.

`uv run pytest --platforms -v` requires Docker and network access. It starts a separate Ubuntu 22.04 container, installs the documented distribution packages, copies only source/build inputs, and runs that same packaging/consumer matrix using Python 3.12. Logs go to `build/verification/ubuntu-22.04.log`. The Fedora checks use the host's native dependencies. Platform build success does not establish runtime camera coverage on both distributions.

```sh
uv run pytest tests/test_recovery.py tests/test_media.py::test_native_media_memory \
    --dataset --native-preset asan -v
uv run pytest tests/test_resilience.py::test_native_replay_decoding_and_shutdown \
    --resilience --native-preset asan -v
uv run pytest tests/test_recovery.py tests/test_media.py::test_native_media_memory \
    tests/test_sanitizer_diagnostics.py --dataset --native-preset tsan -v
```

These commands select specific native checks; `TEST_SEGMENT` supplies decoding footage and `TEST_REPLAY_SEGMENT` supplies replay footage. The entire labeled dataset suite additionally requires human-confirmed labels. ASan/UBSan exercise native recovery, ownership, decoder buffers and shutdown. If LeakSanitizer explicitly fails because execution is traced, `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1` keeps address/undefined-behavior checking active; report leak checking as unavailable for that run.

Run TSan without broad suppression or `ignore_noninstrumented_modules=1` for signoff. The dependency-only diagnostic links GStreamer/OpenCV without HomeSight and saves warnings to `build/verification/tsan-dependency-only.log`; its successful pytest result means the diagnostic completed, not that concurrency is verified. Retain the unfiltered project findings and investigate them individually. The current Fedora dependency stack reports callback publication, buffer/message handoff and dependency-internal warnings that also occur in the standalone probe. A matching GLib/GStreamer/plugin/FFmpeg instrumented stack was also tested and still reports dependency refcount and decoder findings, including in the standalone probe. A clean concurrency signoff remains outstanding; filtered results do not close that limitation. For a private dependency build, set `TEST_TSAN_LIBRARY_PATH` to its library directory. This affects native child processes only, uses its private plugin directory when present, and isolates the plugin registry. Supply all plugins required by the selected native scenarios; missing plugins are errors.

### Measurements

```sh
uv run pytest tests/test_measurements.py --measurements -v
```

Set `TEST_SEGMENT` to actual footage for call timings and `TEST_RTSP_URL` for the resource run. The call test uses its own MinIO and measures ten `analyze_segment_motion()` calls separately from ten synchronous `store_segment_file()` calls on the same input. Analyzer time includes decode and MOG2 with a retained model. Upload time includes signing, file I/O and PUT, excluding background queue delay/backoff, downloads and cleanup. The report includes all samples, median, nearest-rank p95, codec, dimensions, duration, bytes and platform/native versions. These are observations without performance pass/fail thresholds.

The resource test records one actual camera in a dedicated subprocess for at least five minutes (`TEST_MEASUREMENT_SECONDS`, minimum 300). It samples `/proc` every second: CPU is process user+system time divided by elapsed time, with 100% representing one core; RSS includes native buffers. The subprocess records locally with CPU decode/analysis and continuously acknowledges events. It saves segment sizes/codecs and reconnect counts with the CPU/RSS samples. Results are written to `build/verification/call-timings.json` and `build/verification/single-camera-resources.json`. The five-minute baseline is not a production soak or a multi-camera capacity claim. Final outcomes and remaining signoff requirements are recorded in {download}`IMPLEMENTATION_PROGRESS.md <../IMPLEMENTATION_PROGRESS.md>`.


## Building the documentation

Install Doxygen in addition to the native prerequisites (`sudo apt-get install doxygen` on Ubuntu or `sudo dnf install doxygen` on Fedora). Install the locked documentation tools, then build the site from the repository root:

```sh
uv sync --group docs
uv run --group docs sphinx-build -W --keep-going -b html docs build/docs/html
```

Open `build/docs/html/index.html` for the unified guides and Python/C++ reference. The build imports the actual native package and runs Doxygen automatically; missing libraries, a missing Doxygen executable, or generation warnings fail the build. It does not connect to a camera or S3 endpoint. Python signatures come from the checked-in typing stubs, and descriptions come from binding docstrings. Update both when changing an interface. Add Doxygen comments to public C++ declarations, including units, defaults, ownership, exceptions, and concurrency contracts.

After changing native binding docstrings, rebuild the installed extension before generating its Python reference:

```sh
uv sync --group docs --reinstall-package homesight-core
```

Stub changes invalidate the cached Python API page automatically. C++ header comments are regenerated by Doxygen on each documentation build.

The user guide's architecture and application-flow diagrams use ordinary fenced `mermaid` blocks, rendered by Sphinx through `sphinxcontrib-mermaid`. Update these diagrams when the recording pipeline or event lifecycle changes. HTML pages load the pinned Mermaid 11.12.1 renderer from jsDelivr, so viewing diagrams requires internet access; building HTML requires no Node installation. After editing a diagram, open the generated guide and check that it renders correctly, including on narrow screens. The HTML build alone does not validate Mermaid syntax.

For just the Doxygen C++ HTML/XML output, run:

```sh
doxygen docs/Doxyfile
```

Open `build/docs/doxygen/html/index.html` for the standalone C++ reference. Sphinx reads the XML from `build/docs/doxygen/xml/`. All generated files stay under ignored `build/docs/`; no generated API pages are committed. The Python docs dependencies are isolated in the `docs` dependency group and are not runtime dependencies. Hosting and publication are configured separately.
