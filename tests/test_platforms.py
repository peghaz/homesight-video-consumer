"""Verify the native and Python distributions in an isolated Ubuntu 22.04 image."""

import shutil
import subprocess
import uuid
from pathlib import Path

import pytest


@pytest.mark.platforms
def test_ubuntu_2204_builds_and_packaging(tmp_path: Path) -> None:
    """Run the packaging and public-consumer matrix with Ubuntu's own native packages."""
    root: Path = Path(__file__).resolve().parents[1]
    source: Path = tmp_path / "source"
    source.mkdir()
    for name in (
        "CMakeLists.txt",
        "CMakePresets.json",
        "pyproject.toml",
        "uv.lock",
        "README.md",
        "pytest.ini",
        ".python-version",
    ):
        shutil.copyfile(root / name, source / name)
    for name in ("cmake", "include", "src", "python", "tests"):
        shutil.copytree(root / name, source / name, ignore=shutil.ignore_patterns("__pycache__"))
    uv: str | None = shutil.which("uv")
    assert uv is not None
    shutil.copyfile(uv, tmp_path / "uv")
    (tmp_path / "uv").chmod(0o755)
    script = """set -eu
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential cmake ninja-build pkg-config \
    ca-certificates libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly gstreamer1.0-libav libopencv-dev libsqlite3-dev libcurl4-openssl-dev
mkdir -p /work
cp -a /input/source /work/project
cp /input/uv /usr/local/bin/uv
cat /etc/os-release
c++ --version
cmake --version
pkg-config --modversion gstreamer-1.0 opencv4 sqlite3 libcurl
export CMAKE_BUILD_PARALLEL_LEVEL=4
cd /work/project
uv run --python 3.12 pytest tests/test_packaging.py --packaging -v --basetemp=/work/checks
"""
    name = f"homesight-ubuntu-verification-{uuid.uuid4().hex}"
    log_path: Path = root / "build/verification/ubuntu-22.04.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with log_path.open("w", encoding="utf-8") as log:
            result: subprocess.CompletedProcess[str] = subprocess.run(
                [
                    "docker",
                    "run",
                    "--rm",
                    "--name",
                    name,
                    "-v",
                    f"{tmp_path}:/input:ro,Z",
                    "ubuntu:22.04",
                    "sh",
                    "-c",
                    script,
                ],
                stdout=log,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=1800,
            )
        assert result.returncode == 0, log_path.read_text()[-12000:]
    finally:
        subprocess.run(["docker", "rm", "-f", name], capture_output=True, timeout=30)
