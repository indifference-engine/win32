# Win32

Win32 application host.

## Usage

Your application's build pipeline will need to be configured to compile each C
file in the [src/library](./src/library) directory and recompile every C file
should any H files change.  Then, include each H file in the same directory to
make its corresponding function available.

### Assumptions

- The compilation environment supports C99.

### Functions

| Name             | Description                                                                                         |
| ---------------- | --------------------------------------------------------------------------------------------------- |
| `run_event_loop` | Runs an application event loop, blocking until the window is closed by the user or an error occurs. |

### Application Structure

Applications are built from the following events:

#### Tick

The tick event occurs on a fixed interval, provided in hertz when starting the
application event loop.  This is where simulation logic should be incrementally
advanced.  100Hz works for most games, but games with complex simulation logic
not heavily dependent upon responsiveness to user input (e.g. strategy games)
could reduce this significantly.  This fixed update interval eliminates the need
for delta time calculations and eliminates an entire class of potential bugs.

It additionally generates a short buffer of floating-point (signed unit
interval) stereo audio to be played until the next tick.  The number of samples
per channel is provided when starting the application event loop.

#### Video

The video event occurs whenever the display needs to be refreshed.  It is given
the progress through the current tick as a unit interval.

The video event should not alter the state of the simulation.  Instead, it
should interpolate between the previous and current state so that the rendered
frame can smoothly flow from one simulation update to the next.

The display has a fixed resolution provided when starting the application event
loop, and accepts planar RGB in floating point (unit interval).  At present,
this is converted to an unsigned 8-bit integer per channel per pixel.

### Resource Files

It is recommended to include a [resource file](./src/example/resource.rc), an
[icon](./src/example/example.ico) and a
[manifest](./src/example/manifest.manifest) when building your application.
The `windres` executable in the [makefile](./makefile) is responsible for
including them here.

The effects of this are:

- Prevents "double-scaling" of the application's display on DPI-scaled monitors.
- Gives the executable an icon.
- Gives the executable metadata which can be inspected later on.

### Windows SmartScreen

Unfortunately there is no easy way to work around Windows SmartScreen; the
following options are currently known:

#### Submitting executables to Microsoft for analysis

You can [upload your executables to Microsoft](https://www.microsoft.com/en-us/wdsi/filesubmission)
for analysis, after which (with processing time) this may or may not make the
SmartScreen check go away.

This will need to be repeated for each and every build.

#### Signing your builds

It is possible to sign your executables, but this requires a certificate
recognized by Microsoft which costs hundreds of dollars a year.

## Tests

This library does not have any automated tests, but a simple "smoke test"
example application is included.  This can be found at
[dist/example.exe](dist/example.exe) after executing `make`.

### Dependencies

- Make.
- MinGW-GCC.
- Bash.
- Must be linked to the `dwmapi` library.
- Must be linked to the `winmm` library.
