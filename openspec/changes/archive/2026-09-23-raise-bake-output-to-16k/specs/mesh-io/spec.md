## ADDED Requirements

### Requirement: Baked maps are writable one band at a time

A baked map SHALL be writable to PNG and to EXR one horizontal band of rows at a time, in
ascending row order, so that a map larger than memory reaches disk without ever being held
whole. The file written band by band SHALL be BYTE-IDENTICAL to the file the one-shot writer
produces for the same pixels, whatever the band sizes; a single-channel map SHALL be
expanded to grey RGB on the PNG path exactly as the one-shot path expands it. A write that
fails part-way SHALL be reported at the band that failed, and a writer that is handed more
rows than the image has, or finished with fewer, SHALL report failure rather than produce a
file that claims to be complete.

An export bundle SHALL accept a working-set bound (see surface-baking, "Regioned baking with
a bounded working set"). With a bound of zero it SHALL behave exactly as before; with a
non-zero bound every map SHALL be baked in regions and streamed through the preset's
normal-map green convention and colour-space encoding band by band, and the files SHALL be
byte-identical to those the same bundle writes with no bound. Scratch storage a regioned
bundle needs SHALL live beside the bundle's output, on the disk the host is writing to. The
bundle's report SHALL state, per map, the region height, halo, region count and working set
held. A regioned bundle whose bake is cancelled or fails part-way through a map SHALL remove
every file of that map it had opened — the one cut off mid-stream included — so it leaves no
truncated file whose header claims a complete image, exactly as an unbounded bundle, which
writes a map only once its bake has succeeded, leaves none.

#### Scenario: Band-streamed files equal one-shot files
- **WHEN** a map is written to PNG or EXR in bands of several sizes, at 1, 3 and 4 channels
- **THEN** every file SHALL be byte-identical to the one-shot write of the same pixels

#### Scenario: A bundle under a working-set bound writes the same files
- **WHEN** a preset bundle is written with a working-set bound that splits every map into several regions
- **THEN** every map file SHALL be byte-identical to the one the same bundle writes with no bound, and the report SHALL record each map's regions

#### Scenario: A regioned bundle cancelled mid-stream leaves no partial map
- **WHEN** a regioned bundle is cancelled after a map's file has been opened and some of its bands written
- **THEN** the bundle SHALL report the cancellation and that map's file SHALL NOT exist
