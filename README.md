# ShmRgbViewer

Real-time, strictly read-only viewer for VTD RDB image shared memory.

- Monitors all System V shared memory keys `0x8100`–`0x81ff` (every 0.5 s via `/proc/sysvipc/shm`).
- Opens one window per segment that carries RDB images (`RDB_PKG_ID_IMAGE`), closes it when the
  segment disappears or is recreated.
- Attaches with `SHM_RDONLY`: buffer flags (LOCK / TC / IG) are never modified. Locked buffers are
  skipped and torn copies are discarded.
- Standalone: does not depend on VTD headers or libraries (see `rdb_shm.h`).
- Single instance; `--stop` stops a running instance (see below).

## Build

Requires OpenCV 4 (core, imgproc, imgcodecs, highgui).

```bash
cmake -S . -B build
cmake --build build
```

## Run

```bash
./build/ShmRgbViewer                       # all keys 0x8100-0x81ff
./build/ShmRgbViewer 0x08130 0x08142       # only these keys
./build/ShmRgbViewer --range 0x8100 0x82ff # custom key range
./build/ShmRgbViewer --scale 0.5 --no-flip
```

Keys: `q`/`ESC` quit, `f` toggle vertical flip, `s` save current frames as PNG.

### Starting and stopping

- Stop: `q`/`ESC` in a window, Ctrl+C in the terminal, or `./build/ShmRgbViewer --stop`
  (works when it runs in the background; use `sudo` if it was started as root).
- Only one instance runs at a time (lock file `/tmp/ShmRgbViewer.lock`): starting it again stops
  the running instance first. An instance owned by another user is reported instead.
A window closed by hand reopens with the next new frame; use `q` to quit.

## VTD side

The IG must write images to SHM, e.g. in the IG config:

```xml
<RDBInterface name="MyRDBInterface" printDebugInfo="0" ignoreShmFlags="1">
    <ShmLayoutImage key="0x08130" />
</RDBInterface>
...
<Readback readbackThreadAffinity="3" synchronized="1" rdbInterface="MyRDBInterface">
    <Image outputSlot="color"/>
</Readback>
```

`ignoreShmFlags="1"` is needed because this viewer never clears the TC flag; otherwise the IG
stops after filling both buffers.

Images are read back from OpenGL bottom-up, hence the default vertical flip.
