"""Record separate native call timings and a five-minute single-camera baseline."""

from __future__ import annotations

import json
import math
import os
import platform
import statistics
import subprocess
import sys
import time
import uuid
from pathlib import Path
from typing import TYPE_CHECKING

import pytest
from verification_services import (
    MinioService,
    probe_media,
)

import homesight_core as core

if TYPE_CHECKING:
    from conftest import IntegrationSettings

pytestmark = pytest.mark.measurements


def distribution(values: list[float]) -> dict[str, float | int]:
    """Report median and nearest-rank p95 without a performance pass/fail threshold."""
    ordered: list[float] = sorted(values)
    return {
        "count": len(values),
        "median": statistics.median(values),
        "p95": ordered[math.ceil(0.95 * len(ordered)) - 1],
        "minimum": ordered[0],
        "maximum": ordered[-1],
    }


def platform_versions() -> dict[str, str]:
    """Capture enough native runtime detail to interpret measurements."""
    versions = {"platform": platform.platform(), "python": platform.python_version()}
    for package in ("gstreamer-1.0", "opencv4", "sqlite3", "libcurl"):
        output: subprocess.CompletedProcess[str] = subprocess.run(
            ["pkg-config", "--modversion", package],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
        versions[package] = output.stdout.strip()
    return versions


def write_measurement(name: str, payload: object) -> None:
    """Keep detailed results under the repository's ignored verification directory."""
    destination: Path = Path(__file__).resolve().parents[1] / "build/verification"
    destination.mkdir(parents=True, exist_ok=True)
    (destination / name).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def test_separate_analyzer_and_upload_call_times(
    integration_settings: IntegrationSettings,
    minio_service: MinioService,
) -> None:
    """Time decode+analysis and synchronous S3 PUT separately on the same real file."""
    source: Path | None = integration_settings.segment
    assert source is not None, "Set TEST_SEGMENT to actual camera footage"
    probe = probe_media(source)
    analyzer = core.SegmentMotionAnalyzer()
    analysis = list[float]()
    upload = list[float]()
    storage: core.SegmentStorage = core.create_segment_storage(minio_service.config)
    for _ in range(10):
        started: float = time.perf_counter()
        result: core.MotionAnalysisResult = analyzer.analyze_segment_motion(source)
        analysis.append(time.perf_counter() - started)
        assert result.status != core.MotionAnalysisStatus.failed, result.error
        completed = core.CompletedSegment(
            segment=core.SegmentInfo(
                segment_id=uuid.uuid4().hex,
                camera_name="measurements",
                staging_file_path=source,
                start_epoch_ms=1700000000000,
                end_epoch_ms=1700000000000 + round(probe.format.duration * 1000),
                duration_seconds=probe.format.duration,
                video_codec="h265"
                if probe.streams[0].codec_name == "hevc"
                else probe.streams[0].codec_name,
                file_size_bytes=source.stat().st_size,
            ),
            motion=result,
        )
        started = time.perf_counter()
        uri: str = storage.store_segment_file(completed)
        upload.append(time.perf_counter() - started)
        storage.delete_stored_segment(uri)
    write_measurement(
        "call-timings.json",
        {
            "versions": platform_versions(),
            "input": str(source.resolve()),
            "bytes": source.stat().st_size,
            "media": probe.model_dump(),
            "analyzer_seconds": distribution(analysis),
            "upload_seconds": distribution(upload),
            "analyzer_samples_seconds": analysis,
            "upload_samples_seconds": upload,
            "definitions": {
                "analyzer": "analyze_segment_motion wall time: decode and MOG2; retained model",
                "upload": "store_segment_file wall time: signing, file I/O and PUT; no retry queue",
                "p95": "Nearest-rank percentile; no performance threshold",
            },
        },
    )


def test_five_minute_single_camera_resources(
    integration_settings: IntegrationSettings,
    tmp_path: Path,
) -> None:
    """Sample an isolated recorder process at one-second intervals for at least 300s."""
    assert integration_settings.rtsp_url is not None, "Set TEST_RTSP_URL to the actual camera"
    environment: dict[str, str] = dict(os.environ)
    environment.update(
        OWNED_ROOT=str(tmp_path),
        OWNED_RTSP_URL=integration_settings.rtsp_url.get_secret_value(),
        OWNED_SECONDS=str(integration_settings.measurement_seconds),
    )
    samples = list[dict[str, float]]()
    ticks: int = os.sysconf("SC_CLK_TCK")
    with (tmp_path / "resource-worker.log").open("w") as log:
        process: subprocess.Popen[str] = subprocess.Popen(
            [sys.executable, str(Path(__file__).with_name("recording_worker.py")), "measure"],
            env=environment,
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
        )
        started: float = time.monotonic()
        previous_time = started
        previous_cpu: float | None = None
        try:
            while process.poll() is None:
                now: float = time.monotonic()
                if now - started > integration_settings.measurement_seconds + 45:
                    raise AssertionError("Resource worker exceeded bounded shutdown allowance")
                try:
                    fields: list[str] = Path(f"/proc/{process.pid}/stat").read_text().split()
                    cpu: float = (int(fields[13]) + int(fields[14])) / ticks
                    rss: float = int(fields[23]) * os.sysconf("SC_PAGE_SIZE") / (1024 * 1024)
                except FileNotFoundError:
                    break
                if previous_cpu is not None:
                    samples.append(
                        {
                            "elapsed_seconds": now - started,
                            "cpu_percent_one_core": (cpu - previous_cpu)
                            / (now - previous_time)
                            * 100,
                            "rss_mib": rss,
                        }
                    )
                previous_cpu = cpu
                previous_time = now
                time.sleep(1)
            process.wait(timeout=10)
            assert process.returncode == 0, (tmp_path / "resource-worker.log").read_text()
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
    summary: object = json.loads((tmp_path / "worker-summary.json").read_text())
    assert len(samples) >= 299
    write_measurement(
        "single-camera-resources.json",
        {
            "versions": platform_versions(),
            "worker": summary,
            "samples": samples,
            "cpu_percent_one_core": distribution([row["cpu_percent_one_core"] for row in samples]),
            "rss_mib": distribution([row["rss_mib"] for row in samples]),
            "definition": (
                "Dedicated process; one-second /proc deltas; 100% CPU = one core; "
                "RSS includes native buffers"
            ),
        },
    )
