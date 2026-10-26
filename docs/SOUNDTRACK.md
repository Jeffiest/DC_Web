# `soundtrack.json` specification

The PC port creates `soundtrack/soundtrack.json` on startup if it is missing and leaves an existing file untouched. The `soundtrack/` directory sits beside `save/` and `data/`. If `--save DIR` or `DC_SAVE` selects a save directory, `soundtrack/` is beside that directory. The file maps the game's original sequence filenames to user supplied WAV, FLAC, or Ogg Vorbis recordings. The mapping is used when `audio.soundtrack` is `"custom"` in `save/config.json` or Audio > Soundtrack is set to Custom.

```json
{
  "version": 1,
  "overrides": {
    "e01a_a.sq": {
      "file": "norune.ogg",
      "gain": 1.0,
      "loop": true,
      "loop_start": 12.5,
      "loop_end": 150.0
    }
  }
}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `version` | integer | Required; must be `1`. |
| `overrides` | object | Required; keys are original game sequence filenames such as `e01a_a.sq`. |
| `file` | string | Required for each override; recording path relative to `soundtrack/`. |
| `gain` | number | Optional volume multiplier, default `1.0`, applied along with the game's sequence volume. |
| `loop` | boolean | Optional, default `true`; `false` plays the recording once. |
| `loop_start` | number | Optional start of the repeating section in seconds, default `0`. |
| `loop_end` | number | Optional end of the repeating section in seconds; `0` or omission means the end of the recording. |

An override takes effect the next time its sequence starts. A sequence without an override, an invalid override, or a recording that cannot be decoded plays through the game's synthesizer. The port decodes each recording when first used and keeps its decoded samples in memory. `DC_AUDIO_TRACE=1` prints the original sequence filenames as they play.
