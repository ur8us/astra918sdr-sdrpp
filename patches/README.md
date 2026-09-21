# SDR++ compatibility patches

`sdrpp-resampler-predec.patch` modifies SDR++ commit
`8c9f5ee8fe405775bfcd62c8c8f8c0fc928a64af` to avoid a negative/oversized shift
when computing audio predecimation. `scripts/configure_upstream.py` applies it
idempotently before configuring the build. Keep the upstream source notices
and its GPL-3.0 license when distributing the patched SDR++ code; the module's
MIT license does not replace the upstream license.
