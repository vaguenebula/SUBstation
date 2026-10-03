# Audio setup

*Options › Preferences* (**Ctrl+,**, or click the audio device's name at the right of
the transport bar) chooses the audio device, the MIDI inputs and the plug-in folders.
SUBstation plays through ASIO drivers, or WASAPI shared or exclusive, with the sample
rate and buffer size you choose.

## First start

The first launch uses the system's default output device (WASAPI). Change it here.

## The Audio page

| Setting | What it does |
|---|---|
| *Driver Type* | WASAPI or ASIO. ASIO is greyed out when SUBstation was built without the ASIO SDK (see [../building.md](../building.md)) |
| *Audio Device* | The WASAPI device (*System Default* or one by name), or the ASIO driver, with its **Hardware Setup** button |
| *Output Channels* | ASIO only: which pair of the driver's outputs the master plays on |
| *Sample Rate* | The rates the device offers |
| *Buffer Size* | The buffer sizes the device offers, in samples |
| *Exclusive mode* | WASAPI only: lower latency; other apps are silenced |
| *Audio Threads* | How many threads render tracks at once |

Changes there apply at once, because what a driver offers is only known while it runs.
The line below the settings shows the device running, its sample rate, buffer size and
latency, or the error if it didn't open.

### Audio threads

*Audio Threads* sets how many threads render (one per core but one by default; 1 = no
workers: every track renders on the audio thread). Offline renders and exports use the
same threads.

## ASIO

### Choosing a driver

Set *Driver Type* to ASIO and pick the driver. The *Sample Rate* and *Buffer Size* lists
show what it supports, and *Output Channels* which pair of its outputs the master plays
on (1/2, 3/4, ...; an odd last one plays in mono). A driver that sets its buffer size
itself only offers that one: change it in Hardware Setup.

- **Hardware Setup** opens the driver's own settings (buffer size, clock, routing...).
  When they change there (or in the driver's own app, or its clock follows another
  device), the driver asks for a reset and SUBstation opens it again with the new
  buffer size and sample rate, keeping the outputs.
- The status line shows the input and output latency the driver reports.

### Next time

The program opens the same driver next time. If it can't run as saved (another clock,
fewer outputs), it opens with the driver's own settings; if the driver is gone, with the
system's default output (WASAPI).

### Formats and limits

- Every sample format drivers use is converted (16, 24 and 32-bit integers in any
  alignment, 32 and 64-bit floats, either byte order); DSD drivers aren't supported.
  Integer formats are clipped at full scale.
- Only one ASIO driver can be open in a program (the ASIO callbacks have no way to tell
  drivers apart).

### Inputs

A track's input opens the driver's inputs it needs; every buffer of them reaches the
engine, which meters, monitors and records them (see [recording.md](recording.md)).
WASAPI devices open outputs only, so recording from a microphone or line input needs
an ASIO driver; resampling works with either.

## MIDI

The *MIDI* page lists the MIDI inputs connected, each with a check box. Every input is
used unless turned off here; tracks hear the inputs that are on (all of them, or the
one they choose). **Refresh** finds inputs plugged in (or taken out) since. An input
that could not be opened shows in red, with the reason in its tooltip. See
[midi.md](midi.md#midi-input).

## Plug-ins

The *Plug-ins* page lists the VST3 folders (the standard ones, always searched, and
your own: *Add Folder…*, *Remove*) and has the *Rescan Plug-ins* button with the scan's
progress and results. See [plugins.md](plugins.md#finding-them).

## Changing devices while recording

Changing the audio device (or its sample rate, or a reset by the driver) ends a
recording; what was recorded so far is kept.

---

For developers: [../engine/audio-devices.md](../engine/audio-devices.md),
[../python/engine-bridge.md](../python/engine-bridge.md) (audio/settings.py),
[../ui/README.md](../ui/README.md) (dialogs.py).
