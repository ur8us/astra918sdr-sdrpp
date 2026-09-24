# Astra918 SDR++ source

Read README.md and ../astra918sdr/docs/PROTOCOL.md. Preserve reference projects.
Use portable libusb, bounded queues and separate control/IQ lifecycles.
One designated VFO follows the shared CAT dial. Left-right tuning holds the RF
center and changes the channel offset; center tuning uses zero offset. CAT and
the source Tune button preserve offset. Release UI has no simulator controls.
Default tests are offline. No hardware or frequency-generator access.
Run builds/tests and commit coherent changes. Never push without instruction.
