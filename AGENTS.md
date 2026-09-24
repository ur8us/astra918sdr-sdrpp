# Astra918 SDR++ source

Read README.md and ../astra918sdr/docs/PROTOCOL.md. Preserve reference projects.
Use portable libusb, bounded queues and separate control/IQ lifecycles.
Firmware audio/CAT dial equals waterfall center plus firmware audio offset.
All Radio VFOs are independent. Waterfall center changes preserve audio offset;
audio offset edits preserve the center via command 38. CAT preserves offset.
Release UI has no simulator controls.
Default tests are offline. No hardware or frequency-generator access.
Run builds/tests and commit coherent changes. Never push without instruction.
