# Bar/Fixer speech (`CONV/*.VPK`) — format findings

## STATUS: SOLVED 
The static was caused by `tools/extract_bar_speech.py` decoding the VPK payload
as raw 8-bit PCM (`ffmpeg -f u8`). The payload is actually an **LZW-compressed
Creative VOC file** (confirmed via the WC Encyclopedia + a decode that matches
the `Creative Voice File` magic AND the 2-byte length oracle on every entry).

**Final format**
```
VPK file : dword[0]=filesize; then 4-byte index entries (3-byte offset +
           0x20 flag) slicing per-line entries.
entry    : [0:2] decompressed length (LE u16)
           [2:4] zero (length high word / reserved)
           [4:]  LZW bitstream  ->  a standard Creative VOC (.VOC)
LZW      : LSB-first; 9-bit codes growing to 12-bit max; clear=256, end=257,
           first free code=258; width++ when next_code == (1<<width) (NO early
           change). Output = the VOC; hand it to ffmpeg for PCM (8-bit ~11kHz).
```
`tools/extract_bar_speech.py` now implements this (`_lzw_decode_voc`) and
re-extracted all 1854 WAVs as clean speech (mean abs sample-delta ~3-7 vs the
old static ~85). The whole RE journey below is kept for posterity.

> Key lesson: a 2-minute web search (WC Encyclopedia: *".VPK = LZW-compressed
> VOC"*) beat a from-scratch CPU-emulator dig. Always check prior art first.

## Evidence (how we know it's not PCM)
- Raw entry bytes start `218,156,0,0,0,135,200,41,19,...` — full-scale jumps
  every sample. Real speech PCM is locally smooth.
- ~49% of consecutive samples jump >20000/65536 as u8/s8 PCM → static.
- **Entropy 7.84 bits/byte**, all 256 values present → compressed/encoded data,
  not speech PCM (which clusters heavily around mid-level).
- Tested and **rejected** (none yield a speech-like energy envelope with
  silence gaps): u8 PCM, s8 PCM, byte-DPCM, nibble-DPCM, IMA/DVI ADPCM,
  Creative ADPCM 4-bit / 2.6-bit / 2-bit, µ-law, A-law.

## Container layout (reverse-engineered, confirmed)
VPK file:
```
dword[0]        : total file size (LE u32, == filesize)
index table     : 4-byte entries (3-byte abs offset + 1-byte flag 0x20),
                  read while flag == 0x20
each entry span : data[offset .. next_offset]   (this part IS correct today)
```

Per-entry payload (the part the extractor gets wrong):
```
bytes [0:2]   : decompressed length, LE u16  (per-entry, VARIES)
                -> always 1.2x–1.7x the compressed payload size, i.e. a
                   decompressed-size header. Variable ratio => variable-rate
                   compression (Huffman/LZ-style), NOT fixed-ratio ADPCM.
bytes [2:34]  : 32-byte block, IDENTICAL across every entry in every VPK
                -> a baked-in static decode table / codebook / dictionary.
                   hex: 00000087c82913864e1a3b6540587993664c422369d894d1a001808200292204
bytes [34:]   : compressed bitstream (the actual audio)
```
Cross-checked across AGRRUM1–4, PLERUM5: the 32-byte table is byte-for-byte
identical everywhere, which is the signature of a static-table codec.

## What's still unknown
- The exact decompression algorithm (likely an Origin static-table Huffman/LZ
  producing 8-bit PCM, given the ~1.2–1.7x size ratios).
- Output sample format/rate after decompression (assume 8-bit unsigned mono,
  probably 11025 Hz, pending confirmation).

## RE progress against PRCD.EXE (Ghidra/capstone, 16-bit real-mode)
- The 32-byte static table is **NOT** present in `PRCD.EXE` -> it is genuine
  per-file data that the decoder reads at runtime (not a hardcoded constant).
- No `VPK`/`SPEECH`/`VOC` strings in the EXE; but `CONV` appears as both:
  - a path-table entry `\DATA\CONV\` (string blob @ file off 0x7bff5, next to
    `\DATA\SOUND\`, `\DATA\COCKPITS\`, ... and an extension table
    `SHP/PAL/PRS/SAV/XMI/PFC` @ 0x7bf22), and
  - an inline immediate `66 68 'CONV'` = `push dword 0x564e4f43` at four code
    sites in the overlay pool: 0x8ae1d, 0x94d5e, 0x95e75, 0x97a76.
- The speech loader (function containing 0x94d54) does:
  `build path with "CONV"` -> `near call 0x942dc` -> chain of overlay far-calls
  `lcall 0x210:0x92`, `lcall 0x958:0x75`, `lcall 0x958:0x89`, `lcall 0:0x3ba`.
- `0x942dc` is the **OO resource opener** (C++ vtable: `mov bx,[obj]; lcall
  [bx+0x14]`; pushes `'GAME'` = 0x454d4147 to `lcall 0x6b0:0x2ae`). It opens
  the resource; it is NOT the audio decompressor.
- The actual VPK decompressor lives behind the VROOMM overlay far-calls
  (`seg:off` targets like 0x958:xx). Resolving them statically requires
  reconstructing the Borland VROOMM overlay segment->file-offset map.

## Two viable ways to finish
1. **Static (deep):** reconstruct the VROOMM overlay table, map the `seg:off`
   far-call targets to file offsets, then disassemble the decode loop (look for
   the bit-reader: shifts/masks + the 32-byte table lookup).
2. **Dynamic (fast, recommended):** run `re/dosbox/game_patched` in a
   debug DOSBox, breakpoint right after the speech buffer is decoded, and dump
   the decoded PCM straight from RAM (decompressed length is known from the
   2-byte header, so we know exactly how many bytes to grab). This gives
   ground-truth PCM immediately and lets us validate any reimplemented decoder.

## Static RE outcome: blocked at the overlay boundary (same wall as §7.13)
- MZ header: load module starts at file 0xae00; flat segment convention
  `seg = (file_off - 0xae00) >> 4` (matches docs/ai_model.md §7.13.1 OvrInit).
- The loader's **resident** far-calls resolve cleanly with this base:
  - `lcall 0x000:0x3ba` -> file 0xb1ba (clean thunk: push bp/mov bp,sp/.../retf)
  - `lcall 0x210:0x92`  -> file 0xcf92 (clean wrapper -> call 0xc8b8)
- The loader's **game-code** far-calls do NOT resolve with this base:
  - `lcall 0x958:0x75` -> file 0x143f5 and `lcall 0x958:0x89` -> 0x14409 both
    land *mid-function*; disassembling seg 0x958's start (file 0x14380) yields
    mid-instruction garbage. => segment 0x958 is an overlay logical segment
    whose real address is assigned by the VROOMM manager at load time.
- Per §7.13.1 the overlay directory is heap-allocated at runtime; there is no
  static seg->file table. So crossing from the loader into the decompressor
  needs one runtime data point (overlay segment map, or the decoded buffer).
  DOSBox-X is broken on this Mac, so that dynamic capture is currently blocked.

## Promising DATA-ONLY hypothesis (no EXE needed): Huffman-coded ADPCM
The decompressed-length / payload-size ratio is **variable, 1.2x-1.7x**. A
*fixed*-ratio codec (e.g. 4-bit ADPCM = exactly 2.0x) cannot do that; a variable
ratio is the signature of **variable-length (Huffman/arithmetic) coding**.
- ~1.3-1.7x is exactly what Huffman-coding ~4-6 bits/sample yields.
- The per-file **32-byte** static table = plausibly **16 entries x 2 bytes** =
  a 16-symbol code description (16 symbols == 4-bit ADPCM nibbles/step-indices).
- Decode model to test: Huffman-decode the [34:] bitstream into 4-bit symbols
  using the 32-byte table, run them through an ADPCM reconstruct into 8-bit PCM,
  stop at the 2-byte decompressed length. The 2-byte length is a strong oracle:
  a correct decoder must consume ~all of the payload to emit exactly N samples,
  across all 1854 entries.
- Unknowns to pin down: exact table format (code vs code-length; bit order),
  bit-reader endianness, and the ADPCM step/predictor update.

## Unicorn emulation harness (built; validated) -- tools/priv_emu.py
* `tools/priv_emu.py`: maps PRCD.EXE into a Unicorn 16-bit machine under the 1MB
  real-mode ceiling, applies MZ relocations, and can `call_far()` a function at
  a file offset with C args + a sentinel far-return. **Validated**: it correctly
  executes the resident getter @0x14461 and returns the expected value.
  Modes: `resident` (file 0xAE00..0x7FF60, relocs applied) and `overlay`
  (the pool, for leaf emulation). Instrumentation: per-call write-range + a
  src-read flag (`watch_src=`).
* `re/find_vpk_decoder.py` (gitignored): brute-forces the decoder -- emulates
  every `55 8b ec` prologue on a real VPK entry across several Borland calling
  conventions, with `DS=DGROUP` (e_ss+base) so static step-tables resolve, and
  flags any function that reads the source and writes ~N bytes of speech-like
  8-bit PCM (N = the 2-byte header).

### Emulation results so far (negative, but narrowing)
* **Overlay-leaf scan:** 0 candidates even read the source -- overlay functions
  far-call resident runtime early and fault (resident not co-mapped; overlay
  fixups not applied).
* **Resident scan, DS=DGROUP, buffers above DGROUP:** 0 candidates read src AND
  write ~N. (With buffers *inside* DGROUP the run produced 151 false "speech"
  hits -- all 500KB-1MB memset/corruption spans, filtered out by requiring
  span ~= N and by an energy-gap speech check.)
* Conclusion: the decoder is **overlay code that calls resident helpers and uses
  DGROUP** -- so neither a resident leaf nor an overlay leaf. Running it needs
  the *full runtime*.

### Next step to actually run the decoder
Two options, both real work:
1. **Co-map resident + overlay + apply overlay (FBOV) fixups**, then emulate the
   loader function and let its far-calls resolve. Needs reversing Borland's FBOV
   overlay fixup table (header @ file 0x7ff60).
2. **Boot the Borland C startup in-emulator** with stubbed INT 21h (DOS) +
   INT 3F (overlay mgr) so DGROUP and the overlay directory initialize for real,
   then call the CONV speech loader and dump the decoded buffer. Most robust;
   it's a small DOS shim on top of the existing harness.

## Resolution (what shipped)
The emulator dig (above) was made unnecessary by finding prior art: the WC
Encyclopedia documents `.VPK` as an LZW-compressed VOC. `re/vpk_lzw.py`
brute-forced the exact LZW variant against the VOC-magic + length oracle, and
`tools/extract_bar_speech.py` now does LZW -> VOC -> ffmpeg WAV. Re-extraction
of all 1854 lines yields clean speech.

Note: `tools/priv_emu.py` (the validated Unicorn harness) and the brute-force
scanner are kept as general RE tooling, but are NOT needed for speech anymore.

Sources: wcnews.com/wcpedia/Privateer_File_Formats (".VPK = LZW-compressed
VOC"); HCl (hcl.solsector.net) + DMJC/wctools for Origin format RE background.
