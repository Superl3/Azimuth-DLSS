# Fixed-profile SR expansion

Measured profiles remain defaults. Set AZIMUTH_SR_RENDER to the actual WxH with AZIMUTH_SR_OUTPUT; quality is quality/balanced/performance and AZIMUTH_SR_PRESET is default/J/K/L/M. Wide SDR is experimental. Explicit dimensions are bounded to 3840x2160, both render axes below output and at least one third of output, near-matching aspect (1% tolerance), and actual scene/postprocess dimensions. This does NOT change game render resolution. Configure Azimuth independently; NGX actual optimal range must pass. No new profile is runtime validated. Preset hints use pinned SDK quality-specific creation keys; runtime may override. Default leaves NGX policy unchanged. Restart to change profiles or presets.

The packaged debug launcher exposes these settings through -RenderSize, -Preset, -Quality and -NoJitter. -NoJitter is for an intentional temporal comparison; not a recommendation.
