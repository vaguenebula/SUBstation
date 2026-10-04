"""Analysing audio for the editors: signal maths on what devices' displays stream (numpy, no Qt).

The engine plays parameters; what is worked out here becomes parameters again
(through ProjectEditor), so it saves and undoes like any edit. It isn't the
project model (model/) and isn't real-time: it runs on the UI thread, as an
editor reads its displays.

- sidechain_fit: fitting the Sidechain device's curve to a kick."""
