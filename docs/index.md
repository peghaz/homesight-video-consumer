# HomeSight Core

Record RTSP camera video, analyze motion, and sample frames from C++ or Python. This documentation brings together the application guides and API references generated from the public headers and installed Python bindings.

## Record a camera

Install the [native dependencies and Python package](user-guide.md#installation), replace the example RTSP URL, and run the README's recording example:

````{literalinclude} ../README.md
:language: python
:start-after: ```python
:end-before: ```
````

Completed stored clips appear under `recordings/front_door/`. The context manager stops recording on exit; retain staging between runs so pending work can resume. For durable application integration, read [recording events](user-guide.md#recording-events).

## Guides and reference

```{toctree}
:maxdepth: 2

user-guide
python-api
cpp-api
development
```

Doxygen generates the C++ API from `include/homesight/`. Sphinx autodoc reads the installed Python extension, using the public typing stubs for signatures. No camera or S3 connection is required to generate documentation.
