"""Serve existing camera footage on a pytest-owned loopback RTSP endpoint."""

import argparse
import json
from pathlib import Path

import gi  # type: ignore[import-not-found]

gi.require_version("Gst", "1.0")
gi.require_version("GstRtspServer", "1.0")
from gi.repository import GLib, Gst, GstRtspServer  # type: ignore[import-not-found]  # noqa: E402


def main() -> None:
    """Run until pytest terminates this process; never connect to the real camera."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("port", type=int)
    parser.add_argument("codec", choices=["h264", "h265"])
    args: argparse.Namespace = parser.parse_args()
    Gst.init(None)
    server = GstRtspServer.RTSPServer.new()
    server.set_address("127.0.0.1")
    server.set_service(str(args.port))
    factory = GstRtspServer.RTSPMediaFactory.new()
    demux = "matroskademux" if args.source.suffix.lower() == ".mkv" else "qtdemux"
    factory.set_launch(
        f"( filesrc location={json.dumps(str(args.source))} ! {demux} ! "
        f"{args.codec}parse ! rtp{args.codec}pay name=pay0 pt=96 config-interval=-1 )"
    )
    server.get_mount_points().add_factory("/camera", factory)
    if server.attach(None) == 0:
        raise RuntimeError("Cannot attach isolated RTSP server")
    print("RTSP ready", flush=True)
    GLib.MainLoop().run()


if __name__ == "__main__":
    main()
