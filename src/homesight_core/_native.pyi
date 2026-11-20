from enum import Enum
from os import PathLike
from pathlib import Path
from types import TracebackType
from typing import Self

import numpy as np
from numpy.typing import NDArray

__version__: str
type FilePath = str | PathLike[str]

class RecorderError(Exception): ...
class InvalidConfigError(RecorderError): ...
class SegmentDecodingError(RecorderError): ...
class SegmentStorageError(RecorderError): ...
class JournalFullError(RecorderError): ...

class VideoContainerFormat(Enum):
    mp4 = 0
    matroska = 1

class RtspTransport(Enum):
    tcp = 0
    udp = 1

class MotionAnalysisStatus(Enum):
    valid = 0
    warming_up = 1
    insufficient_data = 2
    failed = 3

class SegmentStorageOutcome(Enum):
    pending = 0
    stored = 1
    storage_failed = 2
    deleted_without_motion = 3
    deleted_by_quota = 4
    interrupted = 5

class RecorderEventKind(Enum):
    segment_completed = 0
    storage_changed = 1
    deleted_by_quota = 2

class RecorderState(Enum):
    stopped = 0
    connecting = 1
    recording = 2
    reconnecting = 3
    stopping = 4
    failed = 5

class _Value:
    def to_dict(self) -> dict[str, object]: ...
    @classmethod
    def from_dict(cls, values: dict[str, object]) -> Self: ...

class CameraStreamConfig(_Value):
    camera_name: str
    rtsp_url: str
    rtsp_transport: RtspTransport
    rtsp_latency_ms: int
    def __init__(
        self,
        *,
        camera_name: str = ...,
        rtsp_url: str = ...,
        rtsp_transport: RtspTransport = ...,
        rtsp_latency_ms: int = ...,
    ) -> None: ...

class MotionAnalysisParams(_Value):
    sample_rate_fps: float
    analysis_frame_width: int
    warmup_seconds: float
    history_frames: int
    variance_threshold: float
    learning_rate: float
    changed_area_threshold: float
    minimum_frames_with_motion: int
    exclude_shadows: bool
    use_gpu_decoding: bool
    decode_timeout_seconds: float
    def __init__(
        self,
        *,
        sample_rate_fps: float = ...,
        analysis_frame_width: int = ...,
        warmup_seconds: float = ...,
        history_frames: int = ...,
        variance_threshold: float = ...,
        learning_rate: float = ...,
        changed_area_threshold: float = ...,
        minimum_frames_with_motion: int = ...,
        exclude_shadows: bool = ...,
        use_gpu_decoding: bool = ...,
        decode_timeout_seconds: float = ...,
    ) -> None: ...

class LocalDirectoryStorageConfig(_Value):
    root_directory: Path
    def __init__(self, *, root_directory: FilePath = ...) -> None: ...

class S3Credentials(_Value):
    access_key_id: str
    secret_access_key: str
    session_token: str
    def __init__(
        self, *, access_key_id: str = ..., secret_access_key: str = ..., session_token: str = ...
    ) -> None: ...

class S3StorageConfig(_Value):
    bucket_name: str
    key_prefix: str
    region: str
    endpoint_url: str
    use_path_style_addressing: bool
    verify_tls_certificates: bool
    credentials: S3Credentials
    delete_staging_file_after_upload: bool
    request_timeout_seconds: float
    retry_initial_delay_seconds: float
    retry_max_delay_seconds: float
    def __init__(
        self,
        *,
        bucket_name: str = ...,
        key_prefix: str = ...,
        region: str = ...,
        endpoint_url: str = ...,
        use_path_style_addressing: bool = ...,
        verify_tls_certificates: bool = ...,
        credentials: S3Credentials = ...,
        delete_staging_file_after_upload: bool = ...,
        request_timeout_seconds: float = ...,
        retry_initial_delay_seconds: float = ...,
        retry_max_delay_seconds: float = ...,
    ) -> None: ...

type SegmentStorageConfig = LocalDirectoryStorageConfig | S3StorageConfig

class SegmentRecorderConfig(_Value):
    camera: CameraStreamConfig
    storage: SegmentStorageConfig
    staging_directory: Path
    segment_duration_seconds: float
    container_format: VideoContainerFormat
    motion_analysis: MotionAnalysisParams
    analyze_motion_after_segment_closes: bool
    delete_segments_without_motion: bool
    reconnect_initial_delay_seconds: float
    reconnect_max_delay_seconds: float
    network_timeout_seconds: float
    local_video_allowance_bytes: int
    journal_allowance_bytes: int
    def __init__(
        self,
        *,
        camera: CameraStreamConfig = ...,
        storage: SegmentStorageConfig = ...,
        staging_directory: FilePath = ...,
        segment_duration_seconds: float = ...,
        container_format: VideoContainerFormat = ...,
        motion_analysis: MotionAnalysisParams = ...,
        analyze_motion_after_segment_closes: bool = ...,
        delete_segments_without_motion: bool = ...,
        reconnect_initial_delay_seconds: float = ...,
        reconnect_max_delay_seconds: float = ...,
        network_timeout_seconds: float = ...,
        local_video_allowance_bytes: int = ...,
        journal_allowance_bytes: int = ...,
    ) -> None: ...

class SegmentInfo(_Value):
    segment_id: str
    camera_name: str
    staging_file_path: Path
    start_epoch_ms: int
    end_epoch_ms: int
    duration_seconds: float
    media_start_seconds: float
    video_codec: str
    file_size_bytes: int
    continuity_id: str
    def __init__(
        self,
        *,
        segment_id: str = ...,
        camera_name: str = ...,
        staging_file_path: FilePath = ...,
        start_epoch_ms: int = ...,
        end_epoch_ms: int = ...,
        duration_seconds: float = ...,
        media_start_seconds: float = ...,
        video_codec: str = ...,
        file_size_bytes: int = ...,
        continuity_id: str = ...,
    ) -> None: ...

class MotionAnalysisResult(_Value):
    status: MotionAnalysisStatus
    has_motion: bool | None
    peak_changed_area: float
    mean_changed_area: float
    frames_with_motion: int
    frames_analyzed: int
    params_used: MotionAnalysisParams
    error: str | None
    def __init__(
        self,
        *,
        status: MotionAnalysisStatus = ...,
        has_motion: bool | None = ...,
        peak_changed_area: float = ...,
        mean_changed_area: float = ...,
        frames_with_motion: int = ...,
        frames_analyzed: int = ...,
        params_used: MotionAnalysisParams = ...,
        error: str | None = ...,
    ) -> None: ...

class CompletedSegment(_Value):
    segment: SegmentInfo
    motion: MotionAnalysisResult | None
    motion_analysis_error: str | None
    storage_outcome: SegmentStorageOutcome
    storage_uri: str | None
    storage_error: str | None
    state_revision: int
    def __init__(
        self,
        *,
        segment: SegmentInfo = ...,
        motion: MotionAnalysisResult | None = ...,
        motion_analysis_error: str | None = ...,
        storage_outcome: SegmentStorageOutcome = ...,
        storage_uri: str | None = ...,
        storage_error: str | None = ...,
        state_revision: int = ...,
    ) -> None: ...

class RecorderEvent:
    @property
    def event_id(self) -> str: ...
    @property
    def sequence(self) -> int: ...
    @property
    def kind(self) -> RecorderEventKind: ...
    @property
    def completed_segment(self) -> CompletedSegment: ...

class RecorderStatistics:
    @property
    def state(self) -> RecorderState: ...
    @property
    def segments_written(self) -> int: ...
    @property
    def segments_analyzed(self) -> int: ...
    @property
    def segments_deleted_without_motion(self) -> int: ...
    @property
    def segments_stored(self) -> int: ...
    @property
    def segments_waiting_for_upload(self) -> int: ...
    @property
    def storage_failures(self) -> int: ...
    @property
    def reconnect_attempts(self) -> int: ...
    @property
    def local_video_bytes(self) -> int: ...
    @property
    def last_segment_closed_epoch_ms(self) -> int: ...
    @property
    def last_error(self) -> str | None: ...

class SegmentMotionAnalyzer:
    def __init__(self, params: MotionAnalysisParams = ...) -> None: ...
    def reset(self) -> None: ...
    def analyze_segment_motion(
        self,
        segment_file_path: FilePath,
        continuity_id: str = ...,
        start_media_seconds: float = ...,
    ) -> MotionAnalysisResult: ...

class SegmentStorage:
    def store_segment_file(self, completed: CompletedSegment) -> str: ...
    def find_stored_segment(self, completed: CompletedSegment) -> str | None: ...
    def delete_stored_segment(self, storage_uri: str) -> None: ...
    def download_segment_to_local_file(self, storage_uri: str, destination: FilePath) -> None: ...

class SegmentRecorder:
    def __init__(self, config: SegmentRecorderConfig) -> None: ...
    def start_recording(self) -> None: ...
    def stop_recording(self, finalize_timeout_ms: int = ...) -> None: ...
    def is_recording(self) -> bool: ...
    def get_config(self) -> SegmentRecorderConfig: ...
    def get_recorder_statistics(self) -> RecorderStatistics: ...
    def take_recorder_events(self, max_count: int = ...) -> list[RecorderEvent]: ...
    def wait_for_next_recorder_event(self, timeout_ms: int) -> RecorderEvent | None: ...
    def acknowledge_recorder_events(self, event_ids: list[str]) -> None: ...
    def take_completed_segments(self, max_count: int = ...) -> list[CompletedSegment]: ...
    def wait_for_next_completed_segment(self, timeout_ms: int) -> CompletedSegment | None: ...
    def __enter__(self) -> Self: ...
    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_value: BaseException | None,
        traceback: TracebackType | None,
    ) -> None: ...

def analyze_segment_motion(
    segment_file_path: FilePath, params: MotionAnalysisParams = ...
) -> MotionAnalysisResult: ...
def sample_clip_frames_from_segment(
    segment_file_path: FilePath,
    frame_count: int,
    frame_width: int,
    frame_height: int,
    use_gpu_decoding: bool = ...,
    decode_timeout_seconds: float = ...,
) -> NDArray[np.uint8]: ...
def create_segment_storage(config: SegmentStorageConfig) -> SegmentStorage: ...
