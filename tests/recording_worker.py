"""Own a recorder in a separate process for abrupt-crash and resource verification."""

import argparse
import json
import time
from pathlib import Path

from pydantic import Field, SecretStr
from pydantic_settings import BaseSettings, SettingsConfigDict

import homesight_core as core


class WorkerSettings(BaseSettings):
    """Explicit ephemeral settings supplied only by the owning pytest process."""

    model_config = SettingsConfigDict(env_prefix="OWNED_")
    root: Path
    rtsp_url: SecretStr
    endpoint: str = ""
    access_key: SecretStr = SecretStr("")
    secret_key: SecretStr = SecretStr("")
    seconds: float = Field(default=300, ge=300)


def main() -> None:
    """Keep crash work unacknowledged, or finish a timed single-camera capture."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["crash", "measure"])
    args: argparse.Namespace = parser.parse_args()
    settings = WorkerSettings()
    storage: core.LocalDirectoryStorageConfig | core.S3StorageConfig
    storage = core.LocalDirectoryStorageConfig(root_directory=settings.root / "stored")
    if args.mode == "crash":
        storage = core.S3StorageConfig(
            bucket_name="verification",
            endpoint_url=settings.endpoint,
            key_prefix="segments",
            use_path_style_addressing=True,
            request_timeout_seconds=1,
            retry_initial_delay_seconds=0.2,
            retry_max_delay_seconds=1,
            credentials=core.S3Credentials(
                access_key_id=settings.access_key.get_secret_value(),
                secret_access_key=settings.secret_key.get_secret_value(),
            ),
        )
    config = core.SegmentRecorderConfig(
        camera=core.CameraStreamConfig(
            camera_name="camera", rtsp_url=settings.rtsp_url.get_secret_value()
        ),
        storage=storage,
        staging_directory=settings.root / "stage",
        segment_duration_seconds=2 if args.mode == "crash" else 10,
        container_format=core.VideoContainerFormat.matroska,
        network_timeout_seconds=1,
        reconnect_initial_delay_seconds=0.2,
        reconnect_max_delay_seconds=1,
    )
    recorder = core.SegmentRecorder(config)
    identities = list[str]()
    metadata = list[dict[str, str | int | float]]()
    recorder.start_recording()
    started: float = time.monotonic()
    (settings.root / "recording-ready").touch()
    try:
        while args.mode == "crash" or time.monotonic() - started < settings.seconds:
            event: core.RecorderEvent | None = recorder.wait_for_next_recorder_event(timeout_ms=500)
            if event is not None:
                identities.append(event.event_id)
                segment: core.SegmentInfo = event.completed_segment.segment
                metadata.append(
                    {
                        "id": segment.segment_id,
                        "codec": segment.video_codec,
                        "bytes": segment.file_size_bytes,
                        "duration_seconds": segment.duration_seconds,
                    }
                )
                if args.mode == "measure":
                    recorder.acknowledge_recorder_events([event.event_id])
            stats: core.RecorderStatistics = recorder.get_recorder_statistics()
            if stats.state == core.RecorderState.failed:
                raise RuntimeError(stats.last_error)
            if args.mode == "crash" and stats.segments_written >= 2 and identities:
                target: Path = settings.root / "pending.json"
                if not target.exists():
                    temporary: Path = settings.root / "pending.tmp"
                    temporary.write_text(json.dumps(identities))
                    temporary.replace(target)
    finally:
        recorder.stop_recording(finalize_timeout_ms=3000)
    stats = recorder.get_recorder_statistics()
    assert stats.state == core.RecorderState.stopped, stats.last_error
    assert stats.segments_written > 0
    (settings.root / "worker-summary.json").write_text(
        json.dumps(
            {
                "elapsed_seconds": time.monotonic() - started,
                "segments_written": stats.segments_written,
                "reconnect_attempts": stats.reconnect_attempts,
                "segments": metadata,
            },
            indent=2,
        )
        + "\n"
    )


if __name__ == "__main__":
    main()
