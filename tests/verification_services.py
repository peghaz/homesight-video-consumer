"""Lifecycle and assertions for opt-in, isolated native integration services."""

from __future__ import annotations

import os
import socket
import subprocess
import time
import urllib.error
import urllib.request
import uuid
from collections.abc import Callable, Iterator
from pathlib import Path
from typing import TYPE_CHECKING, TextIO

import pytest
from pydantic import BaseModel, Field

import homesight_core as core

if TYPE_CHECKING:
    from conftest import IntegrationSettings


class MediaProbe(BaseModel):
    """Validate the subset of ffprobe metadata used by verification."""

    class Stream(BaseModel):
        """A video stream's codec and dimensions."""

        codec_name: str
        width: int = Field(gt=0)
        height: int = Field(gt=0)

    class Format(BaseModel):
        """The finalized container's presentation duration."""

        duration: float = Field(gt=0)

    streams: list[Stream] = Field(min_length=1, max_length=1)
    format: Format


def probe_media(path: Path) -> MediaProbe:
    """Inspect actual media without trusting an extension or configured codec."""
    result: subprocess.CompletedProcess[str] = subprocess.run(
        [
            "ffprobe",
            "-v",
            "error",
            "-select_streams",
            "v:0",
            "-show_entries",
            "stream=codec_name,width,height:format=duration",
            "-of",
            "json",
            str(path),
        ],
        check=True,
        capture_output=True,
        text=True,
        timeout=30,
    )
    return MediaProbe.model_validate_json(result.stdout)


def wait_until(predicate: Callable[[], bool], *, seconds: float, description: str) -> None:
    """Poll bounded external progress with a diagnostic rather than fixed sleeps."""
    deadline: float = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.1)
    raise AssertionError(f"Timed out waiting for {description}")


def unused_port() -> int:
    """Choose an ephemeral loopback port; startup detects competing bind attempts."""
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def sanitizer_environment(dependency_path: Path, directory: Path) -> dict[str, str]:
    """Isolate instrumented libraries and plugin discovery to a native child process."""
    prefix: Path = dependency_path.resolve(strict=True)
    environment: dict[str, str] = dict(os.environ)
    environment["LD_LIBRARY_PATH"] = str(prefix) + ":" + environment.get("LD_LIBRARY_PATH", "")
    private_plugins: Path = prefix / "gstreamer-1.0"
    if private_plugins.is_dir():
        environment["GST_PLUGIN_SYSTEM_PATH_1_0"] = str(private_plugins)
    else:
        plugins: subprocess.CompletedProcess[str] = subprocess.run(
            ["pkg-config", "--variable=pluginsdir", "gstreamer-1.0"],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
        environment["GST_PLUGIN_SYSTEM_PATH_1_0"] = plugins.stdout.strip()
    environment["GST_REGISTRY_1_0"] = str(directory / "sanitizer-registry.bin")
    environment["GST_REGISTRY_FORK"] = "no"
    return environment


class RtspReplay:
    """Own one replay subprocess, including deliberate interruption and restart."""

    def __init__(self, source: Path, python: Path, directory: Path) -> None:
        """Validate footage before allowing a decoder-dependent test to start."""
        self.source: Path = source.resolve()
        self.python = python
        self.directory = directory
        self.probe: MediaProbe = probe_media(self.source)
        self.codec: str = self.probe.streams[0].codec_name
        assert self.codec in {"h264", "hevc"}, "Replay requires actual H.264/H.265 camera footage"
        self.port: int = unused_port()
        self.url = f"rtsp://127.0.0.1:{self.port}/camera"
        self.process: subprocess.Popen[str] | None = None
        self.log: TextIO | None = None
        self.generation = 0

    def start(self) -> None:
        """Start a fresh session on the same isolated endpoint."""
        assert self.process is None
        self.generation += 1
        log_path: Path = self.directory / f"rtsp-{self.generation}.log"
        self.log = log_path.open("w", encoding="utf-8")
        self.process = subprocess.Popen(
            [
                str(self.python),
                "-u",
                str(Path(__file__).with_name("rtsp_replay.py")),
                str(self.source),
                str(self.port),
                "h265" if self.codec == "hevc" else "h264",
            ],
            stdout=self.log,
            stderr=subprocess.STDOUT,
            text=True,
        )

        def ready() -> bool:
            """Fail immediately when the system interpreter lacks GI/server plugins."""
            assert self.process is not None
            if self.process.poll() is not None:
                raise AssertionError(log_path.read_text())
            return "RTSP ready" in log_path.read_text()

        wait_until(ready, seconds=10, description="pytest-owned RTSP replay startup")

    def stop(self) -> None:
        """Terminate only this fixture's process and release its output handle."""
        if self.process is not None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
            self.process = None
        if self.log is not None:
            self.log.close()
            self.log = None


class MinioService:
    """Own an ephemeral MinIO container; never operate on a pre-existing service."""

    def __init__(self, image: str, directory: Path) -> None:
        """Generate unique container ownership, bucket and disposable credentials."""
        self.name = f"homesight-pytest-{uuid.uuid4().hex}"
        self.image = image
        self.directory = directory
        self.access_key = uuid.uuid4().hex
        self.secret_key = uuid.uuid4().hex
        self.endpoint = ""
        self.port: int = unused_port()
        self.created = False
        self.config = core.S3StorageConfig()

    def command(self, *arguments: str) -> str:
        """Invoke Docker with bounded waits and actionable stderr."""
        result: subprocess.CompletedProcess[str] = subprocess.run(
            ["docker", *arguments], check=True, capture_output=True, text=True, timeout=90
        )
        return result.stdout.strip()

    def start(self) -> None:
        """Create an isolated service and bucket without using the user's credentials."""
        environment: dict[str, str] = dict(os.environ)
        environment.update(MINIO_ROOT_USER=self.access_key, MINIO_ROOT_PASSWORD=self.secret_key)
        subprocess.run(
            [
                "docker",
                "create",
                "--pull=never",
                "--name",
                self.name,
                "-p",
                f"127.0.0.1:{self.port}:9000",
                "-e",
                "MINIO_ROOT_USER",
                "-e",
                "MINIO_ROOT_PASSWORD",
                self.image,
                "server",
                "/data",
            ],
            env=environment,
            check=True,
            capture_output=True,
            text=True,
            timeout=90,
        )
        self.created = True
        self.command("start", self.name)
        port: str = self.command("port", self.name, "9000/tcp").rsplit(":", 1)[1]
        self.endpoint = f"http://127.0.0.1:{port}"
        self.config = core.S3StorageConfig(
            bucket_name="verification",
            endpoint_url=self.endpoint,
            key_prefix="segments",
            use_path_style_addressing=True,
            request_timeout_seconds=1,
            retry_initial_delay_seconds=0.2,
            retry_max_delay_seconds=1,
            credentials=core.S3Credentials(
                access_key_id=self.access_key, secret_access_key=self.secret_key
            ),
        )
        self.wait_ready()
        # libcurl signs a real S3 bucket-creation request. Credentials travel over stdin.
        subprocess.run(
            [
                "curl",
                "--silent",
                "--show-error",
                "--fail",
                "--max-time",
                "10",
                "--aws-sigv4",
                "aws:amz:us-east-1:s3",
                "--config",
                "-",
                "-X",
                "PUT",
                f"{self.endpoint}/verification",
            ],
            input=f'user = "{self.access_key}:{self.secret_key}"\n',
            check=True,
            capture_output=True,
            text=True,
            timeout=15,
        )

    def wait_ready(self) -> None:
        """Wait for the dedicated service's HTTP health endpoint."""

        def ready() -> bool:
            """Treat only expected startup connection failures as not ready."""
            try:
                with urllib.request.urlopen(
                    f"{self.endpoint}/minio/health/ready", timeout=1
                ) as reply:
                    return int(reply.status) == 200
            except (urllib.error.URLError, TimeoutError, ConnectionError):
                return False

        wait_until(ready, seconds=30, description="pytest-owned MinIO readiness")

    def interrupt(self) -> None:
        """Stop this container while retaining its temporary object data."""
        self.command("stop", "-t", "2", self.name)

    def resume(self) -> None:
        """Restore the same endpoint and data after an upload outage."""
        self.command("start", self.name)
        assert self.command("port", self.name, "9000/tcp").endswith(f":{self.port}")
        self.wait_ready()

    def close(self) -> None:
        """Retain diagnostics, then remove only the uniquely named owned container."""
        if self.created:
            try:
                result: subprocess.CompletedProcess[str] = subprocess.run(
                    ["docker", "logs", self.name],
                    check=True,
                    capture_output=True,
                    text=True,
                    timeout=30,
                )
                (self.directory / "minio.log").write_text(
                    result.stdout + result.stderr, encoding="utf-8"
                )
            finally:
                self.command("rm", "-f", "-v", self.name)
                self.created = False


@pytest.fixture
def rtsp_replay(integration_settings: IntegrationSettings, tmp_path: Path) -> Iterator[RtspReplay]:
    """Use explicit actual footage, never the live camera endpoint."""
    source: Path | None = integration_settings.replay_segment
    assert source is not None, "Set TEST_REPLAY_SEGMENT to actual H.264/H.265 footage (>=30s)"
    replay = RtspReplay(source, integration_settings.rtsp_python, tmp_path)
    assert replay.probe.format.duration >= 30, "Replay footage must cover at least 30 seconds"
    try:
        replay.start()
        yield replay
    finally:
        replay.stop()


@pytest.fixture
def minio_service(
    integration_settings: IntegrationSettings, tmp_path: Path
) -> Iterator[MinioService]:
    """Provision, interrupt and remove only a unique pytest-owned MinIO service."""
    service = MinioService(integration_settings.minio_image, tmp_path)
    try:
        service.start()
        yield service
    finally:
        service.close()
