"""Explicit integration selection and compilation of native recovery helpers."""

import os
import re
import shlex
import subprocess
from pathlib import Path

import pytest
from pydantic import Field, SecretStr
from pydantic_settings import BaseSettings, SettingsConfigDict

pytest_plugins = ["verification_services"]


class IntegrationSettings(BaseSettings):
    """Load optional verification inputs once at the pytest boundary."""

    model_config = SettingsConfigDict(
        env_prefix="TEST_",
        env_file=Path(__file__).resolve().parents[1] / ".env",
        extra="ignore",
        env_ignore_empty=True,
    )
    rtsp_url: SecretStr | None = None
    dataset_manifest: Path | None = None
    segment: Path | None = None
    s3_endpoint: str | None = None
    s3_bucket: str | None = None
    s3_access_key: SecretStr | None = None
    s3_secret_key: SecretStr | None = None
    s3_session_token: SecretStr | None = None
    camera_test_seconds: float = Field(default=25, ge=12)
    replay_segment: Path | None = None
    h264_segment: Path | None = None
    h265_segment: Path | None = None
    rtsp_python: Path = Path("/usr/bin/python3")
    minio_image: str = "pgsty/silo:RELEASE.2026-11-12T00-00-00Z"
    measurement_seconds: float = Field(default=300, ge=300)
    tsan_library_path: Path | None = None


def pytest_addoption(parser: pytest.Parser) -> None:
    """Expose deliberate integration selection, including required-input failures."""
    parser.addoption("--live", action="store_true", help="Run actual RTSP camera tests")
    parser.addoption("--dataset", action="store_true", help="Run labeled camera footage tests")
    parser.addoption("--s3", action="store_true", help="Run real S3 endpoint tests")
    parser.addoption(
        "--keep-s3-objects",
        action="store_true",
        help="Keep uploaded S3 test objects after verification, including on failure",
    )
    parser.addoption("--packaging", action="store_true", help="Build and install artifacts")
    parser.addoption("--gpu", action="store_true", help="Verify available GPU decoding")
    parser.addoption("--resilience", action="store_true", help="Run pytest-owned RTSP/MinIO faults")
    parser.addoption("--media", action="store_true", help="Run both codecs and container formats")
    parser.addoption(
        "--measurements", action="store_true", help="Measure calls and a five-minute run"
    )
    parser.addoption("--platforms", action="store_true", help="Build in isolated Ubuntu 22.04")
    parser.addoption(
        "--native-preset",
        default="dev",
        choices=["dev", "release", "asan", "tsan"],
        help="Native preset exercised by the standalone C++ recovery client",
    )


def pytest_collection_modifyitems(config: pytest.Config, items: list[pytest.Item]) -> None:
    """Skip opt-in suites unless explicitly selected by flag or marker expression."""
    selected: set[str] = set(re.findall(r"\b\w+\b", config.getoption("markexpr")))
    for suite in (
        "live",
        "dataset",
        "s3",
        "packaging",
        "gpu",
        "resilience",
        "media",
        "measurements",
        "platforms",
    ):
        enabled = config.getoption(f"--{suite}") or suite in selected
        if not enabled:
            for item in items:
                combined_selected = (
                    suite in {"live", "s3"}
                    and "real_rtsp_s3" in selected
                    and "real_rtsp_s3" in item.keywords
                )
                if combined_selected:
                    continue
                if suite in item.keywords:
                    reason = (
                        "Select -m real_rtsp_s3 (or both --live and --s3) to run"
                        if "real_rtsp_s3" in item.keywords
                        else f"Select --{suite} to run"
                    )
                    item.add_marker(pytest.mark.skip(reason=reason))


@pytest.fixture(scope="session")
def integration_settings() -> IntegrationSettings:
    """Read optional integration configuration at the test boundary."""
    return IntegrationSettings()


@pytest.fixture(scope="session")
def state_driver(
    tmp_path_factory: pytest.TempPathFactory,
    pytestconfig: pytest.Config,
    integration_settings: IntegrationSettings,
) -> Path:
    """Build an internal journal client; pytest remains the only test runner."""
    root: Path = Path(__file__).resolve().parents[1]
    build: Path = tmp_path_factory.mktemp("native-state")
    preset: str = pytestconfig.getoption("--native-preset")
    sanitizer: str = {"asan": "address,undefined", "tsan": "thread"}.get(preset, "")
    (build / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.22)\n"
        "project(StateDriver LANGUAGES CXX)\n"
        "find_package(HomesightCore REQUIRED)\n"
        f'add_executable(state-driver "{root / "tests/state_driver.cpp"}")\n'
        f'target_include_directories(state-driver PRIVATE "{root / "src"}")\n'
        "target_link_libraries(state-driver PRIVATE HomesightCore::homesight_core)\n",
        encoding="utf-8",
    )
    # Compile every installed public header as the first include in its own consumer.
    with (build / "CMakeLists.txt").open("a", encoding="utf-8") as cmake:
        for header in sorted((root / "include/homesight").glob("*.hpp")):
            consumer: Path = build / f"header_{header.stem}.cpp"
            consumer.write_text(f"#include <homesight/{header.name}>\n", encoding="utf-8")
            cmake.write(
                f'add_library(header_{header.stem} OBJECT "{consumer}")\n'
                f"target_link_libraries(header_{header.stem} PRIVATE "
                "HomesightCore::homesight_core)\n"
            )
    if sanitizer:
        with (build / "CMakeLists.txt").open("a", encoding="utf-8") as cmake:
            cmake.write(
                f"target_compile_options(state-driver PRIVATE -fsanitize={sanitizer})\n"
                f"target_link_options(state-driver PRIVATE -fsanitize={sanitizer})\n"
            )
    native: Path = root / "build" / preset
    subprocess.run(
        ["cmake", "--preset", preset], cwd=root, check=True, capture_output=True, text=True
    )
    subprocess.run(
        ["cmake", "--build", "--preset", preset, "-j", "4"],
        cwd=root,
        check=True,
        capture_output=True,
        text=True,
    )
    prefix: Path = build / "prefix"
    subprocess.run(
        ["cmake", "--install", str(native), "--prefix", str(prefix)],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run(
        ["cmake", "-S", str(build), "-B", str(build / "out"), f"-DCMAKE_PREFIX_PATH={prefix}"],
        check=True,
        capture_output=True,
        text=True,
    )
    subprocess.run(
        ["cmake", "--build", str(build / "out"), "-j", "2"],
        check=True,
        capture_output=True,
        text=True,
    )
    executable: Path = build / "out/state-driver"
    if preset == "tsan" and integration_settings.tsan_library_path is not None:
        from verification_services import sanitizer_environment

        environment: dict[str, str] = sanitizer_environment(
            integration_settings.tsan_library_path, build
        )
        launcher: Path = build / "state-driver-with-dependencies"
        variables = (
            "LD_LIBRARY_PATH",
            "GST_PLUGIN_SYSTEM_PATH_1_0",
            "GST_REGISTRY_1_0",
            "GST_REGISTRY_FORK",
        )
        launcher.write_text(
            "#!/bin/sh\n"
            + "".join(f"export {key}={shlex.quote(environment[key])}\n" for key in variables)
            + f'exec {shlex.quote(str(executable))} "$@"\n',
            encoding="utf-8",
        )
        launcher.chmod(0o755)
        return launcher
    return executable


@pytest.fixture
def child_environment() -> dict[str, str]:
    """Preserve native loader and dependency configuration in child processes."""
    return dict(os.environ)
