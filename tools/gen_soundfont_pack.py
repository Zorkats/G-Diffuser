#!/usr/bin/env python3
"""Build, scaffold and validate soundfont overlay packs (GFT1 v1) for the audio-pack pipeline.

Quick start:

    # scaffold an editable project prefilled from the stock font dump:
    python tools/gen_soundfont_pack.py init FONT_DDBGM_BIG_BLUE -o my-font/

    # edit my-font/font.json (and drop in your own WAVs), then pack:
    python tools/gen_soundfont_pack.py pack my-font/ -o my-font.o2r --name "My Font" --author you

    # validate a project or an existing pack without writing anything:
    python tools/gen_soundfont_pack.py check my-font/
    python tools/gen_soundfont_pack.py check my-font.o2r

Project layout ("font pack project"):
  <dir>/
    font.json         the descriptor this packer consumes (see below).
    samples/*.gsmp    pre-encoded GSMP blobs (written by init; referenced as {"gsmp": ...}).
    <name>.wav        modder-supplied PCM WAVs (referenced as {"wav": ...}, encoded at pack
                      time through the stage-3 VADPCM pipeline in tools/gen_sample_pack.py).
    workshop.json     (optional) pack metadata; synthesized from the --name/--author/--version/
                      --id flags when absent (same rules as gen_sequence_pack.py).

font.json shape:
  {
    "font": "FONT_DDBGM_BIG_BLUE",            // one of the 23 names (see `init --names`)
    "envelopes": { "lead_env": [[24, 32767], [-1, 0]] },
    "samples":    { "lead": { "wav": "lead.wav", "rateHz": 22050,
                              "loop": { "start": 0, "end": 115700 } },
                    "stock_one": { "gsmp": "samples/stock_one.gsmp" } },
    "instruments": [ { "index": 0, "rangeLo": 0, "rangeHi": 127, "adsrDecayIndex": 250,
                       "envelope": "lead_env",
                       "normal": { "sample": "lead", "tuning": 0.6890625 },
                       "low": null, "high": null } ],
    "drums": [], "sfx": []
  }

Rules mirrored from the runtime (the packer rejects what the runtime rejects):
  - instrument "index" covers exactly 0..numInstruments-1; counts (instruments/drums/sfx) must
    equal the stock font when dump/audio/fonts/ek_<FONT>.json is available.
  - all of low/normal/high null = empty slot (sampleRefNormal = -1); a low sound is required
    iff rangeLo != 0, a high sound iff rangeHi != 0x7F.
  - envelope delay semantics: >0 normal, 0 = ADSR_DISABLE, -1 = ADSR_HANG, -2 = ADSR_GOTO
    (arg < pointCount), -3 = ADSR_RESTART; at least one terminating/hanging/goto point must
    occur within pointCount.
  - numEnvelopes 1..64, numSamples 1..512, per-sample decodedLength <= 8 MiB.
  - every embedded GSMP blob must round-trip through gen_sample_pack.parse_gsmp.

Output: one .o2r (ZIP) holding the font blob at archive path "audio/font/<FONTNAME>" plus
workshop.json at the root (pack_type "soundfont"). The archive is written deterministically
(sorted entries, fixed 1980 timestamps) so identical inputs produce byte-identical output.

Container ("GFT1", version 1, ALL little-endian, hand-packed with struct.pack):
  0  magic "GFT1" | 4 version u16=1 | 6 reserved u16=0 | 8 stockFontId u16 (fontIndex)
  10 numInstruments u8 | 11 numDrums u8 | 12 numSfx u8 | 13 numEnvelopes u8 | 14 numSamples u16
  16 instruments x 24 bytes:
       sampleRefLow s16 | sampleRefNormal s16 | sampleRefHigh s16 | envRef s16 (all -1 = absent)
       rangeLo u8 | rangeHi u8 | adsrDecayIndex u8 | reserved u8=0
       tuningLow f32 | tuningNormal f32 | tuningHigh f32
  .. drums x 12 bytes: sampleRef s16 | envRef s16 | adsrDecayIndex u8 | pan u8 | reserved u16=0
       | tuning f32
  .. sfx x 8 bytes: sampleRef s16 | reserved u16=0 | tuning f32
  .. envelopes x variable: pointCount u8 | reserved u8=0 | reserved u16=0,
       points x { delay s16, arg s16 }
  .. sampleTable x 8 bytes: gsmpOffset u32 (absolute, 4-aligned) | gsmpSize u32
  .. gsmp blob area (each blob is gen_sample_pack.pack_gsmp output)
  .. crc32 u32 (zlib.crc32 of every preceding byte; computed last)

KNOWN DATA LIMITATION (init): the EK font dumps carry no waveform data for the DD LBA sample
banks (dump/audio/manifest.tsv: "LBA banks metadata-only"), and envelope dumps carry only the
point COUNT, not the points. init therefore prefills:
  - samples that share a cart bank with a dumped WAV (dump/audio/samples/<key>.wav): re-encoded
    from that WAV through the lossy VADPCM pipeline (~20-25 dB SNR — the raw stock ADPCM blobs
    are not dumped, so a byte-exact passthrough is impossible);
  - samples with no dumped WAV: a short SILENT placeholder GSMP, so the project packs and loads
    but plays silence until the modder drops in a real WAV;
  - a default envelope ("stock_env": [[24, 32767], [-1, 0]]) on every instrument.
Stock loop points are floored to a 16-sample boundary (GSMP requires frame-aligned loop starts).
"""
import argparse
import importlib.util
import json
import os
import re
import struct
import sys
import zipfile
import zlib

FIXED_DATE = (1980, 1, 1, 0, 0, 0)

GFT1_MAGIC = b"GFT1"
GFT1_VERSION = 1
GFT1_HEADER_SIZE = 16
INST_SIZE = 24
DRUM_SIZE = 12
SFXREC_SIZE = 8
SAMPLE_TABLE_ENTRY_SIZE = 8
MAX_ENVELOPES = 64
MAX_ENV_POINTS = 64
MAX_SAMPLES = 512
MAX_DECODED_LENGTH = 8 * 1024 * 1024

ADSR_DISABLE = 0
ADSR_HANG = -1
ADSR_GOTO = -2
ADSR_RESTART = -3

# The 23 replaceable fonts, in FontId order (decomp/include/sfx.h, EXPANSION_KIT branch:
# FONT_GUITAR=0 .. FONT_DDBGM_EAD_DEMO=22). Matches dump/audio/fonts/ek_<NAME>.json fontIndex.
FONT_NAMES = [
    "FONT_GUITAR",
    "FONT_SOUND_EFFECTS",
    "FONT_DDBGM_MUTE_CITY",
    "FONT_DDBGM_SILENCE",
    "FONT_DDBGM_SAND_OCEAN",
    "FONT_DDBGM_PORT_TOWN",
    "FONT_DDBGM_BIG_BLUE",
    "FONT_DDBGM_DEVILS_FOREST",
    "FONT_DDBGM_RED_CANYON",
    "FONT_DDBGM_SECTOR",
    "FONT_DDBGM_WHITE_LAND",
    "FONT_DDBGM_RAINBOW_ROAD",
    "FONT_DDBGM_NEW_03",
    "FONT_DDBGM_NEW_02",
    "FONT_DDBGM_NEW_01",
    "FONT_DDBGM_NEW_04",
    "FONT_DDBGM_TITLE",
    "FONT_DDBGM_SELECT",
    "FONT_DDBGM_OPTION",
    "FONT_DDBGM_DEATHRACE",
    "FONT_DDBGM_COURSE_EDITOR",
    "FONT_DDBGM_MACHINE_EDITOR",
    "FONT_DDBGM_EAD_DEMO",
]
FONT_NAME_SET = frozenset(FONT_NAMES)

SAMPLE_NAME_RE = re.compile(r"^[A-Za-z0-9_]+$")

# Default envelope used by init (stock envelope point DATA is not dumped, only the point count):
# fast attack to full scale, then hang.
DEFAULT_ENV_POINTS = [[24, 32767], [ADSR_HANG, 0]]
DEFAULT_ENV_NAME = "stock_env"
PLACEHOLDER_SAMPLES = 4096  # silent placeholder length for stock samples with no dumped WAV

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_DUMP_AUDIO = os.path.join(_REPO_ROOT, "dump", "audio")


def _load_sample_packer():
    """Import tools/gen_sample_pack.py by path (it has a __main__ guard). The whole VADPCM
    pipeline (load_wav, resample_linear, design_book, encode_adpcm, snr_db, pack_gsmp,
    parse_gsmp) is reused from there — never duplicated here."""
    path = os.path.join(_REPO_ROOT, "tools", "gen_sample_pack.py")
    spec = importlib.util.spec_from_file_location("gen_sample_pack", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


gsp = _load_sample_packer()
validate_pack_id = gsp.validate_pack_id


# ════════════════════════════════════════════════════════════════════════════════════════════════
# Dump ground truth.
# ════════════════════════════════════════════════════════════════════════════════════════════════
def load_font_dump(fontname):
    """Return the parsed dump/audio/fonts/ek_<FONTNAME>.json doc, or None when absent."""
    path = os.path.join(_DUMP_AUDIO, "fonts", "ek_%s.json" % fontname)
    if not os.path.isfile(path):
        return None
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def stock_sample_wav(key):
    """Path of the dumped WAV for a stock sample key, or None."""
    path = os.path.join(_DUMP_AUDIO, "samples", key + ".wav")
    return path if os.path.isfile(path) else None


# ════════════════════════════════════════════════════════════════════════════════════════════════
# Sample encoding (thin orchestration over gen_sample_pack's VADPCM pipeline).
# ════════════════════════════════════════════════════════════════════════════════════════════════
def _loop_state(decoded, start):
    """16-s16 predictor state at the loop point (temporal order, state[15] newest), matching
    gen_sample_pack.encode_one / the runtime's A_LOOP history seeding."""
    if start < gsp.FRAME_SAMPLES:
        return [0] * (gsp.FRAME_SAMPLES - start) + decoded[:start]
    return decoded[start - gsp.FRAME_SAMPLES:start]


def encode_wav_sample(name, wav_path, loop_req, rates, npred):
    """Encode one WAV into a GSMP blob. Resamples to the stock rate only when the name is a
    known stock sample key (modder-named samples keep their own rate; pitch is the instrument
    tuning's job). loop_req = (start, end) in decoded samples of the FINAL (post-resample)
    signal. Returns (blob, report_line, warnings). Raises ValueError."""
    warnings = []
    try:
        samples, rate = gsp.load_wav(wav_path)
    except ValueError as exc:
        raise ValueError("sample '%s': %s: %s" % (name, os.path.basename(wav_path), exc))
    if not samples:
        raise ValueError("sample '%s': empty WAV (0 frames)" % name)

    target = rates.get(name)
    if target is not None and target != rate:
        samples = gsp.resample_linear(samples, rate, target)
        rate_note = "resampled %d -> %d Hz (stock rate)" % (rate, target)
    else:
        rate_note = "rate %d Hz" % rate

    coefs, order, npred_out = gsp.design_book(samples, 2, npred)
    payload, decoded = gsp.encode_adpcm(samples, order, npred_out, coefs)
    decoded_length = len(decoded)

    if loop_req is not None:
        start, end = loop_req
        if start % gsp.FRAME_SAMPLES != 0:
            raise ValueError("sample '%s': loop.start %d is not frame-aligned (%% 16 != 0)"
                             % (name, start))
        if not (0 <= start < end <= decoded_length):
            raise ValueError("sample '%s': loop bounds invalid: start %d, end %d, "
                             "decodedLength %d (need 0 <= start < end <= decodedLength)"
                             % (name, start, end, decoded_length))
        loop = (start, end, gsp.LOOP_FOREVER, _loop_state(decoded, start))
    else:
        loop = (0, 0, 0, [0] * gsp.FRAME_SAMPLES)

    padded_ref = samples + [0] * (decoded_length - len(samples))
    snr = gsp.snr_db(padded_ref, decoded)
    blob = gsp.pack_gsmp(payload, decoded_length, loop, order, npred_out, coefs)
    report = ("sample '%s': %d frames, order %d, %d predictor(s), %s, SNR %s dB%s"
              % (name, decoded_length // gsp.FRAME_SAMPLES, order, npred_out, rate_note,
                 "inf" if snr == float("inf") else "%.1f" % snr,
                 ", loop %d..%d" % loop_req if loop_req else ""))
    if snr < 20.0 and snr != float("inf"):
        warnings.append("sample '%s': SNR %.1f dB is below 20 dB — the encode is audibly lossy"
                        % (name, snr))
    return blob, report, warnings


def encode_placeholder(rate, npred):
    """A short silent one-shot GSMP, used by init for stock samples that have no dumped WAV
    (the DD LBA banks carry no waveform data in this repo)."""
    samples = [0] * PLACEHOLDER_SAMPLES
    coefs, order, npred_out = gsp.design_book(samples, 2, npred)
    payload, decoded = gsp.encode_adpcm(samples, order, npred_out, coefs)
    return gsp.pack_gsmp(payload, len(decoded), (0, 0, 0, [0] * gsp.FRAME_SAMPLES),
                         order, npred_out, coefs)


# ════════════════════════════════════════════════════════════════════════════════════════════════
# Envelope validation.
# ════════════════════════════════════════════════════════════════════════════════════════════════
def validate_envelope(name, points):
    """Validate one envelope's points. Returns a list of error strings (empty = valid).
    delay: >0 normal, 0 DISABLE, -1 HANG, -2 GOTO (arg < pointCount), -3 RESTART. At least one
    terminating/hanging/goto point must occur within pointCount."""
    errors = []
    if not isinstance(points, list) or not 1 <= len(points) <= MAX_ENV_POINTS:
        return ["envelope '%s': pointCount must be 1..%d, got %s"
                % (name, MAX_ENV_POINTS,
                   len(points) if isinstance(points, list) else type(points).__name__)]
    count = len(points)
    has_terminator = False
    for i, pt in enumerate(points):
        if not isinstance(pt, (list, tuple)) or len(pt) != 2 \
                or not all(isinstance(v, int) for v in pt):
            errors.append("envelope '%s' point %d: must be [delay, arg] integers" % (name, i))
            continue
        delay, arg = pt
        if not -32768 <= delay <= 32767 or not -32768 <= arg <= 32767:
            errors.append("envelope '%s' point %d: delay/arg out of s16 range (%d, %d)"
                          % (name, i, delay, arg))
            continue
        if delay == ADSR_GOTO:
            if not 0 <= arg < count:
                errors.append("envelope '%s' point %d: ADSR_GOTO arg %d out of range "
                              "(need 0 <= arg < pointCount %d)" % (name, i, arg, count))
            has_terminator = True
        elif delay in (ADSR_DISABLE, ADSR_HANG, ADSR_RESTART):
            has_terminator = True
        elif delay < ADSR_RESTART:
            errors.append("envelope '%s' point %d: delay %d is not a valid ADSR command "
                          "(>0, 0=DISABLE, -1=HANG, -2=GOTO, -3=RESTART)" % (name, i, delay))
    if not has_terminator:
        errors.append("envelope '%s': no terminating/hanging/goto point within pointCount %d "
                      "(need at least one delay of 0, -1, -2 or -3)" % (name, count))
    return errors


# ════════════════════════════════════════════════════════════════════════════════════════════════
# Project loading / validation / GFT1 building.
# ════════════════════════════════════════════════════════════════════════════════════════════════
def _pitch_ref(inst, which, errors, sample_index):
    """Resolve a low/normal/high pitch entry to (sampleRef, tuning). Entry is null (absent,
    ref -1) or {"sample": name, "tuning": float}."""
    entry = inst.get(which)
    label = "instrument %d %s" % (inst.get("index", "?"), which)
    if entry is None:
        return -1, 0.0
    if not isinstance(entry, dict) or not isinstance(entry.get("sample"), str):
        errors.append("%s: must be null or {\"sample\": <name>, \"tuning\": <float>}" % label)
        return -1, 0.0
    sname = entry["sample"]
    if sname not in sample_index:
        errors.append("%s references unknown sample '%s'" % (label, sname))
        return -1, 0.0
    tuning = entry.get("tuning", 1.0)
    if not isinstance(tuning, (int, float)):
        errors.append("%s: tuning must be a number, got %r" % (label, tuning))
        return -1, 0.0
    return sample_index[sname], float(tuning)


def load_project(input_dir):
    """Read and structurally validate font.json. Returns (doc, errors)."""
    path = os.path.join(input_dir, "font.json")
    if not os.path.isfile(path):
        return None, ["no font.json in %s (run `init <FONTNAME> -o <dir>` to scaffold one)"
                      % input_dir]
    try:
        with open(path, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
    except json.JSONDecodeError as exc:
        return None, ["font.json is not valid JSON (%s)" % exc]
    if not isinstance(doc, dict):
        return None, ["font.json must be a JSON object"]
    errors = []
    font = doc.get("font")
    if font not in FONT_NAME_SET:
        errors.append('font.json "font" %r is not one of the 23 known font names '
                      "(see `init --names`)" % (font,))
    for key in ("envelopes", "samples"):
        if not isinstance(doc.get(key), dict):
            errors.append('font.json "%s" must be an object (name -> definition)' % key)
    if not isinstance(doc.get("instruments"), list):
        errors.append('font.json "instruments" must be an array')
    for key in ("drums", "sfx"):
        if doc.get(key) is not None and not isinstance(doc.get(key), list):
            errors.append('font.json "%s" must be an array' % key)
    return doc, errors


def build_gft1(input_dir, npred, encode=True):
    """Validate the project and build the GFT1 blob. With encode=False only structure is
    checked (WAVs are header-checked but not encoded; gsmp entries are still fully parsed —
    used by `check <dir>`). Returns (blob, fontname, errors, warnings, reports)."""
    errors = []
    warnings = []
    reports = []

    doc, lerr = load_project(input_dir)
    if lerr:
        return None, None, lerr, warnings, reports

    fontname = doc["font"]
    dump = load_font_dump(fontname)
    font_index = FONT_NAMES.index(fontname)
    if dump is not None:
        if dump.get("fontIndex") != font_index:
            errors.append("dump json fontIndex %d != enum index %d for %s (table bug)"
                          % (dump.get("fontIndex"), font_index, fontname))
        stock_counts = (dump.get("numInstruments"), dump.get("numDrums"), dump.get("numSfx"))
    else:
        warnings.append("no dump json for %s — stock count cross-check skipped" % fontname)
        stock_counts = None

    # ── envelopes (sorted by name for determinism) ──────────────────────────────────────────
    env_names = sorted(doc["envelopes"])
    env_index = {}
    env_blobs = []
    for name in env_names:
        points = doc["envelopes"][name]
        errors.extend(validate_envelope(name, points))
        env_index[name] = len(env_blobs)
        env_blobs.append([(pt[0], pt[1]) for pt in points] if isinstance(points, list)
                         and all(isinstance(p, (list, tuple)) and len(p) == 2 for p in points)
                         else [])
    if not 1 <= len(env_blobs) <= MAX_ENVELOPES:
        errors.append("numEnvelopes %d out of range (1..%d)" % (len(env_blobs), MAX_ENVELOPES))

    # ── samples (sorted by name for determinism) ────────────────────────────────────────────
    _known_keys, rates = gsp.load_dump_info()
    sample_names = sorted(doc["samples"])
    for name in sample_names:
        if not SAMPLE_NAME_RE.match(name):
            errors.append("sample name %r must match [A-Za-z0-9_]+" % name)
    sample_index = {name: i for i, name in enumerate(sample_names)}
    if not 1 <= len(sample_names) <= MAX_SAMPLES:
        errors.append("numSamples %d out of range (1..%d)" % (len(sample_names), MAX_SAMPLES))

    sample_blobs = [None] * len(sample_names)
    for name in sample_names:
        entry = doc["samples"][name]
        if not isinstance(entry, dict):
            errors.append("sample '%s': definition must be an object" % name)
            continue
        has_wav = "wav" in entry
        has_gsmp = "gsmp" in entry
        if has_wav == has_gsmp:
            errors.append("sample '%s': define exactly one of \"wav\" or \"gsmp\"" % name)
            continue
        if has_gsmp:
            path = os.path.join(input_dir, entry["gsmp"])
            if not os.path.isfile(path):
                errors.append("sample '%s': gsmp file not found: %s" % (name, entry["gsmp"]))
                continue
            with open(path, "rb") as fh:
                blob = fh.read()
            info, gerr = gsp.parse_gsmp(blob)
            for msg in gerr:
                errors.append("sample '%s' (%s): %s" % (name, entry["gsmp"], msg))
            if info is not None and not gerr:
                if info["decodedLength"] > MAX_DECODED_LENGTH:
                    errors.append("sample '%s': decodedLength %d exceeds 8 MiB" % name)
                else:
                    sample_blobs[sample_index[name]] = blob
                    reports.append("sample '%s': gsmp %d bytes, %d decoded samples%s"
                                   % (name, len(blob), info["decodedLength"],
                                      ", loop %d..%d" % info["loop"][:2]
                                      if info["loop"][2] else ""))
            continue
        # wav entry
        path = os.path.join(input_dir, entry["wav"])
        if not os.path.isfile(path):
            errors.append("sample '%s': wav file not found: %s" % (name, entry["wav"]))
            continue
        loop_req = None
        loop = entry.get("loop")
        if loop is not None:
            if not isinstance(loop, dict) or not isinstance(loop.get("start"), int) \
                    or not isinstance(loop.get("end"), int):
                errors.append('sample \'%s\': "loop" must be {"start": int, "end": int} '
                              '(decoded-sample units)' % name)
                continue
            loop_req = (loop["start"], loop["end"])
        rate_hz = entry.get("rateHz")
        if rate_hz is not None and not isinstance(rate_hz, int):
            errors.append("sample '%s': rateHz must be an integer" % name)
            continue
        if not encode:
            # check-only: header-load the WAV and validate loop bounds against the padded
            # (post-resample) decoded length without running the encoder.
            try:
                samples, rate = gsp.load_wav(path)
            except ValueError as exc:
                errors.append("sample '%s': %s: %s" % (name, os.path.basename(path), exc))
                continue
            if not samples:
                errors.append("sample '%s': empty WAV (0 frames)" % name)
                continue
            target = rates.get(name)
            if target is not None and target != rate:
                samples = gsp.resample_linear(samples, rate, target)
            decoded_length = -(-len(samples) // gsp.FRAME_SAMPLES) * gsp.FRAME_SAMPLES
            if decoded_length > MAX_DECODED_LENGTH:
                errors.append("sample '%s': decodedLength %d exceeds 8 MiB"
                              % (name, decoded_length))
            if loop_req is not None:
                start, end = loop_req
                if start % gsp.FRAME_SAMPLES != 0:
                    errors.append("sample '%s': loop.start %d is not frame-aligned (%% 16 != 0)"
                                  % (name, start))
                elif not (0 <= start < end <= decoded_length):
                    errors.append("sample '%s': loop bounds invalid: start %d, end %d, "
                                  "decodedLength %d" % (name, start, end, decoded_length))
            reports.append("sample '%s': %d samples @ %d Hz -> %d decoded samples%s"
                           % (name, len(samples), rate, decoded_length,
                              ", loop %d..%d" % loop_req if loop_req else ", one-shot"))
            continue
        try:
            blob, report, warns = encode_wav_sample(name, path, loop_req, rates, npred)
        except ValueError as exc:
            errors.append(str(exc))
            continue
        decoded_length = struct.unpack_from("<I", blob, 16)[0]
        if decoded_length > MAX_DECODED_LENGTH:
            errors.append("sample '%s': decodedLength %d exceeds 8 MiB" % (name, decoded_length))
            continue
        warnings.extend(warns)
        reports.append(report)
        sample_blobs[sample_index[name]] = blob

    # ── instruments ─────────────────────────────────────────────────────────────────────────
    instruments = doc["instruments"]
    num_inst = len(instruments)
    if stock_counts is not None and num_inst != stock_counts[0]:
        errors.append("numInstruments %d does not equal the stock count %d for %s "
                      "(a font overlay must keep the stock layout)"
                      % (num_inst, stock_counts[0], fontname))
    if num_inst > 255:
        errors.append("numInstruments %d exceeds 255" % num_inst)
    seen_idx = set()
    inst_records = []
    referenced_samples = set()
    referenced_envs = set()
    for inst in instruments:
        if not isinstance(inst, dict):
            errors.append("instrument entry must be an object, got %s" % type(inst).__name__)
            continue
        idx = inst.get("index")
        if not isinstance(idx, int) or not 0 <= idx < num_inst:
            errors.append("instrument index %r out of range (must cover exactly 0..%d)"
                          % (idx, num_inst - 1))
            continue
        if idx in seen_idx:
            errors.append("instrument index %d is defined twice" % idx)
            continue
        seen_idx.add(idx)

        range_lo = inst.get("rangeLo")
        range_hi = inst.get("rangeHi")
        decay = inst.get("adsrDecayIndex")
        if not isinstance(range_lo, int) or not 0 <= range_lo <= 127:
            errors.append("instrument %d: rangeLo must be 0..127" % idx)
            continue
        if not isinstance(range_hi, int) or not 0 <= range_hi <= 127:
            errors.append("instrument %d: rangeHi must be 0..127" % idx)
            continue
        if range_lo > range_hi:
            errors.append("instrument %d: rangeLo %d > rangeHi %d" % (idx, range_lo, range_hi))
            continue
        if not isinstance(decay, int) or not 0 <= decay <= 255:
            errors.append("instrument %d: adsrDecayIndex must be 0..255" % idx)
            continue

        low_ref, low_tuning = _pitch_ref(inst, "low", errors, sample_index)
        norm_ref, norm_tuning = _pitch_ref(inst, "normal", errors, sample_index)
        high_ref, high_tuning = _pitch_ref(inst, "high", errors, sample_index)
        for ref in (low_ref, norm_ref, high_ref):
            if ref >= 0:
                referenced_samples.add(ref)

        # low sound required iff rangeLo != 0; high sound required iff rangeHi != 0x7F.
        if range_lo != 0 and low_ref < 0:
            errors.append("instrument %d: rangeLo %d != 0 requires a \"low\" sound" %
                          (idx, range_lo))
        if range_lo == 0 and low_ref >= 0:
            errors.append("instrument %d: \"low\" sound present but rangeLo is 0 "
                          "(a low ref is only meaningful when rangeLo != 0)" % idx)
        if range_hi != 0x7F and high_ref < 0:
            errors.append("instrument %d: rangeHi %d != 0x7F requires a \"high\" sound"
                          % (idx, range_hi))
        if range_hi == 0x7F and high_ref >= 0:
            errors.append("instrument %d: \"high\" sound present but rangeHi is 0x7F "
                          "(a high ref is only meaningful when rangeHi != 0x7F)" % idx)

        env_name = inst.get("envelope")
        if env_name is None:
            env_ref = -1
        elif env_name not in env_index:
            errors.append("instrument %d references unknown envelope '%s'" % (idx, env_name))
            env_ref = -1
        else:
            env_ref = env_index[env_name]
            referenced_envs.add(env_ref)
        inst_records.append((idx, low_ref, norm_ref, high_ref, env_ref,
                             range_lo, range_hi, decay, low_tuning, norm_tuning, high_tuning))
    missing_idx = sorted(set(range(num_inst)) - seen_idx)
    if missing_idx:
        errors.append("instrument indices missing: %s (must cover exactly 0..%d)"
                      % (", ".join(str(i) for i in missing_idx), num_inst - 1))

    # ── drums / sfx (no EK font uses these, but the container supports them) ────────────────
    drum_records = []
    for i, drum in enumerate(doc.get("drums") or []):
        if not isinstance(drum, dict):
            errors.append("drum %d: entry must be an object" % i)
            continue
        sname = drum.get("sample")
        sref = -1
        if sname is not None:
            if sname not in sample_index:
                errors.append("drum %d references unknown sample '%s'" % (i, sname))
            else:
                sref = sample_index[sname]
                referenced_samples.add(sref)
        env_name = drum.get("envelope")
        eref = -1
        if env_name is not None:
            if env_name not in env_index:
                errors.append("drum %d references unknown envelope '%s'" % (i, env_name))
            else:
                eref = env_index[env_name]
                referenced_envs.add(eref)
        decay = drum.get("adsrDecayIndex", 0)
        pan = drum.get("pan", 64)
        tuning = drum.get("tuning", 1.0)
        if not isinstance(decay, int) or not 0 <= decay <= 255:
            errors.append("drum %d: adsrDecayIndex must be 0..255" % i)
            continue
        if not isinstance(pan, int) or not 0 <= pan <= 255:
            errors.append("drum %d: pan must be 0..255" % i)
            continue
        if not isinstance(tuning, (int, float)):
            errors.append("drum %d: tuning must be a number" % i)
            continue
        drum_records.append((sref, eref, decay, pan, float(tuning)))
    sfx_records = []
    for i, sfx in enumerate(doc.get("sfx") or []):
        if not isinstance(sfx, dict):
            errors.append("sfx %d: entry must be an object" % i)
            continue
        sname = sfx.get("sample")
        sref = -1
        if sname is not None:
            if sname not in sample_index:
                errors.append("sfx %d references unknown sample '%s'" % (i, sname))
            else:
                sref = sample_index[sname]
                referenced_samples.add(sref)
        tuning = sfx.get("tuning", 1.0)
        if not isinstance(tuning, (int, float)):
            errors.append("sfx %d: tuning must be a number" % i)
            continue
        sfx_records.append((sref, float(tuning)))
    if stock_counts is not None:
        if len(drum_records) != stock_counts[1]:
            errors.append("numDrums %d does not equal the stock count %d for %s"
                          % (len(drum_records), stock_counts[1], fontname))
        if len(sfx_records) != stock_counts[2]:
            errors.append("numSfx %d does not equal the stock count %d for %s"
                          % (len(sfx_records), stock_counts[2], fontname))

    for i, name in enumerate(sample_names):
        if i not in referenced_samples:
            warnings.append("sample '%s' is defined but never referenced (packed anyway)" % name)
    for i, name in enumerate(env_names):
        if i not in referenced_envs:
            warnings.append("envelope '%s' is defined but never referenced" % name)

    if errors:
        return None, fontname, errors, warnings, reports
    if not encode:
        return None, fontname, errors, warnings, reports

    # ── emit ────────────────────────────────────────────────────────────────────────────────
    inst_records.sort(key=lambda r: r[0])
    out = bytearray()
    out += GFT1_MAGIC
    out += struct.pack("<HH", GFT1_VERSION, 0)
    out += struct.pack("<H", font_index)
    out += struct.pack("<BBBBH", num_inst, len(drum_records), len(sfx_records),
                       len(env_blobs), len(sample_names))
    for (_idx, low_ref, norm_ref, high_ref, env_ref, range_lo, range_hi, decay,
         low_t, norm_t, high_t) in inst_records:
        out += struct.pack("<hhhhBBBBfff", low_ref, norm_ref, high_ref, env_ref,
                           range_lo, range_hi, decay, 0, low_t, norm_t, high_t)
    for (sref, eref, decay, pan, tuning) in drum_records:
        out += struct.pack("<hhBBHf", sref, eref, decay, pan, 0, tuning)
    for (sref, tuning) in sfx_records:
        out += struct.pack("<hHf", sref, 0, tuning)
    for points in env_blobs:
        out += struct.pack("<BBH", len(points), 0, 0)
        for delay, arg in points:
            out += struct.pack("<hh", delay, arg)

    table_off = len(out)
    blob_off = table_off + SAMPLE_TABLE_ENTRY_SIZE * len(sample_names)
    table = bytearray()
    blobs = bytearray()
    for blob in sample_blobs:
        pad = (-(blob_off + len(blobs))) % 4
        blobs += b"\x00" * pad
        table += struct.pack("<II", blob_off + len(blobs), len(blob))
        blobs += blob
    out += table
    out += blobs
    out += struct.pack("<I", zlib.crc32(bytes(out)) & 0xFFFFFFFF)

    blob = bytes(out)
    info, perr = parse_gft1(blob, check_gsmp=True)
    if perr:
        errors.extend("internal self-check: %s" % m for m in perr)
        return None, fontname, errors, warnings, reports
    reports.append("font %s (stockFontId %d): %d instrument(s), %d envelope(s), %d sample(s), "
                   "%d bytes" % (fontname, font_index, num_inst, len(env_blobs),
                                 len(sample_names), len(blob)))
    return blob, fontname, errors, warnings, reports


# ════════════════════════════════════════════════════════════════════════════════════════════════
# GFT1 parsing (used by `check` and by pack's self-verification).
# ════════════════════════════════════════════════════════════════════════════════════════════════
def parse_gft1(blob, check_gsmp=False):
    """Parse and validate a GFT1 blob. Returns (info, errors); info is None on structural
    failure. check_gsmp also runs every embedded blob through gen_sample_pack.parse_gsmp."""
    errors = []
    if len(blob) < GFT1_HEADER_SIZE + 4:
        return None, ["blob too small for GFT1 header (%d bytes)" % len(blob)]
    if blob[:4] != GFT1_MAGIC:
        return None, ["bad magic %r (want GFT1)" % blob[:4]]
    version, reserved = struct.unpack_from("<HH", blob, 4)
    font_id = struct.unpack_from("<H", blob, 8)[0]
    num_inst, num_drums, num_sfx, num_env, num_samples = \
        struct.unpack_from("<BBBBH", blob, 10)

    if version != GFT1_VERSION:
        errors.append("unsupported version %d (want %d)" % (version, GFT1_VERSION))
    if reserved != 0:
        errors.append("reserved field must be 0, got %d" % reserved)
    if font_id >= len(FONT_NAMES):
        errors.append("stockFontId %d out of range (0..%d)" % (font_id, len(FONT_NAMES) - 1))
    if not 1 <= num_env <= MAX_ENVELOPES:
        errors.append("numEnvelopes %d out of range (1..%d)" % (num_env, MAX_ENVELOPES))
    if not 1 <= num_samples <= MAX_SAMPLES:
        errors.append("numSamples %d out of range (1..%d)" % (num_samples, MAX_SAMPLES))
    if errors:
        return None, errors

    off = GFT1_HEADER_SIZE
    instruments = []
    for i in range(num_inst):
        if off + INST_SIZE > len(blob):
            return None, ["truncated instrument table at instrument %d" % i]
        (low_ref, norm_ref, high_ref, env_ref, range_lo, range_hi, decay, rsv,
         low_t, norm_t, high_t) = struct.unpack_from("<hhhhBBBBfff", blob, off)
        off += INST_SIZE
        for label, ref in (("sampleRefLow", low_ref), ("sampleRefNormal", norm_ref),
                           ("sampleRefHigh", high_ref)):
            if ref < -1 or ref >= num_samples:
                errors.append("instrument %d: %s %d out of range (-1..%d)"
                              % (i, label, ref, num_samples - 1))
        if env_ref < -1 or env_ref >= num_env:
            errors.append("instrument %d: envRef %d out of range (-1..%d)"
                          % (i, env_ref, num_env - 1))
        if rsv != 0:
            errors.append("instrument %d: reserved byte must be 0, got %d" % (i, rsv))
        if range_lo != 0 and low_ref < 0:
            errors.append("instrument %d: rangeLo %d != 0 but sampleRefLow is -1"
                          % (i, range_lo))
        if range_hi != 0x7F and high_ref < 0:
            errors.append("instrument %d: rangeHi %d != 0x7F but sampleRefHigh is -1"
                          % (i, range_hi))
        instruments.append({"low": (low_ref, low_t), "normal": (norm_ref, norm_t),
                            "high": (high_ref, high_t), "envRef": env_ref,
                            "rangeLo": range_lo, "rangeHi": range_hi,
                            "adsrDecayIndex": decay})
    drums = []
    for i in range(num_drums):
        if off + DRUM_SIZE > len(blob):
            return None, ["truncated drum table at drum %d" % i]
        sref, eref, decay, pan, rsv, tuning = struct.unpack_from("<hhBBHf", blob, off)
        off += DRUM_SIZE
        if sref < -1 or sref >= num_samples:
            errors.append("drum %d: sampleRef %d out of range" % (i, sref))
        if eref < -1 or eref >= num_env:
            errors.append("drum %d: envRef %d out of range" % (i, eref))
        if rsv != 0:
            errors.append("drum %d: reserved u16 must be 0, got %d" % (i, rsv))
        drums.append({"sampleRef": sref, "envRef": eref, "adsrDecayIndex": decay,
                      "pan": pan, "tuning": tuning})
    sfxs = []
    for i in range(num_sfx):
        if off + SFXREC_SIZE > len(blob):
            return None, ["truncated sfx table at sfx %d" % i]
        sref, rsv, tuning = struct.unpack_from("<hHf", blob, off)
        off += SFXREC_SIZE
        if sref < -1 or sref >= num_samples:
            errors.append("sfx %d: sampleRef %d out of range" % (i, sref))
        if rsv != 0:
            errors.append("sfx %d: reserved u16 must be 0, got %d" % (i, rsv))
        sfxs.append({"sampleRef": sref, "tuning": tuning})
    envelopes = []
    for i in range(num_env):
        if off + 4 > len(blob):
            return None, ["truncated envelope table at envelope %d" % i]
        count, rsv1, rsv2 = struct.unpack_from("<BBH", blob, off)
        off += 4
        if rsv1 != 0 or rsv2 != 0:
            errors.append("envelope %d: reserved bytes must be 0, got %d/%d" % (i, rsv1, rsv2))
        if not 1 <= count <= MAX_ENV_POINTS:
            errors.append("envelope %d: pointCount %d out of range (1..%d)"
                          % (i, count, MAX_ENV_POINTS))
            break
        if off + 4 * count > len(blob):
            return None, ["truncated envelope %d points" % i]
        points = [struct.unpack_from("<hh", blob, off + 4 * j) for j in range(count)]
        off += 4 * count
        errors.extend(validate_envelope("<blob #%d>" % i, points))
        envelopes.append(points)
    if off + SAMPLE_TABLE_ENTRY_SIZE * num_samples > len(blob):
        return None, ["truncated sample table"]
    sample_table = []
    for i in range(num_samples):
        goff, gsize = struct.unpack_from("<II", blob, off)
        off += SAMPLE_TABLE_ENTRY_SIZE
        if goff % 4 != 0:
            errors.append("sample %d: gsmpOffset %d is not 4-aligned" % (i, goff))
        if goff + gsize > len(blob) - 4:
            errors.append("sample %d: gsmp [%d, %d) overruns the blob (%d bytes before crc)"
                          % (i, goff, goff + gsize, len(blob) - 4))
        sample_table.append((goff, gsize))
    if off > len(blob) - 4:
        return None, ["tables overrun the blob"]

    stored_crc = struct.unpack_from("<I", blob, len(blob) - 4)[0]
    actual_crc = zlib.crc32(blob[:len(blob) - 4]) & 0xFFFFFFFF
    if stored_crc != actual_crc:
        errors.append("crc32 mismatch: stored 0x%08X, computed 0x%08X"
                      % (stored_crc, actual_crc))

    gsmp_infos = []
    for i, (goff, gsize) in enumerate(sample_table):
        if goff % 4 != 0 or goff + gsize > len(blob) - 4:
            gsmp_infos.append(None)
            continue
        sub = blob[goff:goff + gsize]
        if check_gsmp:
            info, gerr = gsp.parse_gsmp(sub)
            for msg in gerr:
                errors.append("sample %d: embedded GSMP invalid: %s" % (i, msg))
            if info is not None and info["decodedLength"] > MAX_DECODED_LENGTH:
                errors.append("sample %d: decodedLength %d exceeds 8 MiB"
                              % (i, info["decodedLength"]))
            gsmp_infos.append(info)
        else:
            gsmp_infos.append({"decodedLength": struct.unpack_from("<I", sub, 16)[0]
                               if len(sub) >= 20 else None})

    info = {
        "version": version, "stockFontId": font_id, "numInstruments": num_inst,
        "numDrums": num_drums, "numSfx": num_sfx, "numEnvelopes": num_env,
        "numSamples": num_samples, "instruments": instruments, "drums": drums, "sfx": sfxs,
        "envelopes": envelopes, "sampleTable": sample_table, "gsmp": gsmp_infos,
        "crc32": stored_crc, "size": len(blob),
    }
    return info, errors


# ════════════════════════════════════════════════════════════════════════════════════════════════
# init — scaffold an editable project from the stock font dump.
# ════════════════════════════════════════════════════════════════════════════════════════════════
def _pitch_entry(pitch):
    if pitch is None:
        return None
    return {"sample": pitch["sample"], "tuning": pitch["tuning"]}


def cmd_init(args):
    if args.font is None or args.output_dir is None:
        sys.stderr.write("error: init requires <FONTNAME> and -o <dir> (or pass --names)\n")
        return 2
    if args.font not in FONT_NAME_SET:
        sys.stderr.write("error: %r is not one of the 23 known font names (see `init --names`)\n"
                         % args.font)
        return 2
    dump = load_font_dump(args.font)
    if dump is None:
        sys.stderr.write("error: no dump data for %s (dump/audio/fonts/ek_%s.json missing)\n"
                         % (args.font, args.font))
        return 2
    outdir = args.output_dir
    if os.path.isfile(os.path.join(outdir, "font.json")) and not args.force:
        sys.stderr.write("error: %s already contains a font.json (use --force to overwrite)\n"
                         % outdir)
        return 2
    os.makedirs(os.path.join(outdir, "samples"), exist_ok=True)

    _known_keys, rates = gsp.load_dump_info()
    warnings = []
    sample_entries = {}
    n_reencoded = 0
    n_placeholder = 0
    for key in sorted(dump.get("samples") or {}):
        meta = dump["samples"][key]
        wav = None if args.skip_samples else stock_sample_wav(key)
        gsmp_rel = os.path.join("samples", key + ".gsmp")
        gsmp_abs = os.path.join(outdir, gsmp_rel)
        if wav is not None:
            loop_req = None
            loop = meta.get("loop") or {}
            if loop.get("count"):
                # GSMP requires frame-aligned loop starts; floor the stock point (the
                # re-encode is lossy anyway, so sub-frame loop fidelity is already gone).
                start = int(loop["start"])
                start -= start % gsp.FRAME_SAMPLES
                end = int(loop["end"])
                loop_req = (start, end)
            try:
                blob, report, warns = encode_wav_sample(key, wav, loop_req, rates,
                                                        args.predictors)
            except ValueError as exc:
                # Stock loop end can exceed the padded decoded length on re-encode; retry
                # without the loop rather than failing the scaffold.
                if loop_req is not None:
                    warnings.append("%s: dropped stock loop %d..%d (%s)"
                                    % (key, loop_req[0], loop_req[1], exc))
                    try:
                        blob, report, warns = encode_wav_sample(key, wav, None, rates,
                                                                args.predictors)
                    except ValueError as exc2:
                        sys.stderr.write("error: %s\n" % exc2)
                        return 1
                else:
                    sys.stderr.write("error: %s\n" % exc)
                    return 1
            warnings.extend(warns)
            print("  %s" % report)
            n_reencoded += 1
        else:
            blob = encode_placeholder(rates.get(key, 32000), args.predictors)
            n_placeholder += 1
        with open(gsmp_abs, "wb") as fh:
            fh.write(blob)
        entry = {"gsmp": gsmp_rel.replace(os.sep, "/")}
        rate = rates.get(key) or (meta.get("playbackRateHz")
                                  if isinstance(meta.get("playbackRateHz"), int) else None)
        if rate:
            entry["rateHz"] = int(rate)
        sample_entries[key] = entry

    instruments = []
    for inst in dump.get("instruments") or []:
        instruments.append({
            "index": inst["index"],
            "rangeLo": inst["normalRangeLo"],
            "rangeHi": inst["normalRangeHi"],
            "adsrDecayIndex": inst["adsrDecayIndex"],
            "envelope": DEFAULT_ENV_NAME,
            "normal": _pitch_entry(inst.get("normalPitch")),
            "low": _pitch_entry(inst.get("lowPitch")),
            "high": _pitch_entry(inst.get("highPitch")),
        })

    font_doc = {
        "_comment": ("Scaffolded from dump/audio/fonts/ek_%s.json. Samples were RE-ENCODED "
                     "from the dumped WAVs through the lossy VADPCM pipeline (the raw stock "
                     "ADPCM bank blobs are not dumped); stock samples with no dumped WAV (the "
                     "DD LBA banks carry no waveform data in this repo) are SILENT "
                     "placeholders — replace them with your own WAVs ({\"wav\": \"name.wav\"} "
                     "entries). The stock envelope point DATA is not dumped (only the point "
                     "count), so every instrument starts on the default '%s' envelope. "
                     "Instrument/drum/sfx counts and indices must stay at the stock layout."
                     % (args.font, DEFAULT_ENV_NAME)),
        "font": args.font,
        "envelopes": {DEFAULT_ENV_NAME: DEFAULT_ENV_POINTS},
        "samples": sample_entries,
        "instruments": instruments,
        "drums": [],
        "sfx": [],
    }
    with open(os.path.join(outdir, "font.json"), "w", encoding="utf-8") as fh:
        json.dump(font_doc, fh, indent=2)
        fh.write("\n")

    workshop = {
        "name": args.font,
        "version": "0.1",
        "author": "unknown",
        "game_version": "us.rev0",
        "id": "gd." + args.font.lower(),
        "pack_type": "soundfont",
    }
    with open(os.path.join(outdir, "workshop.json"), "w", encoding="utf-8") as fh:
        json.dump(workshop, fh, indent=2, sort_keys=True)
        fh.write("\n")

    print("wrote %s: %d instrument(s), %d sample(s) (%d re-encoded from dumped WAVs, "
          "%d silent placeholders)" % (outdir, len(instruments), len(sample_entries),
                                       n_reencoded, n_placeholder))
    for msg in warnings:
        print("warn:  %s" % msg)
    if n_placeholder:
        sys.stderr.write(
            "note: %d stock sample(s) have no dumped WAV (DD LBA banks carry no waveform data "
            "in this repo) — they are SILENT placeholders; replace them with your own WAVs.\n"
            % n_placeholder)
    if n_reencoded:
        sys.stderr.write(
            "note: %d stock sample(s) were re-encoded from dumped WAVs through the lossy "
            "VADPCM pipeline (~20-25 dB SNR); the raw stock ADPCM blobs are not dumped, so a "
            "byte-exact passthrough is impossible.\n" % n_reencoded)
    sys.stderr.write("note: stock envelope point data is not dumped — every instrument uses "
                     "the default '%s' envelope; edit font.json to taste.\n" % DEFAULT_ENV_NAME)
    return 0


# ════════════════════════════════════════════════════════════════════════════════════════════════
# pack / check.
# ════════════════════════════════════════════════════════════════════════════════════════════════
def resolve_manifest_bytes(args, input_dir):
    """Return (manifest_bytes, source_description). Same rules as gen_sequence_pack.py:
    on-disk workshop.json/manifest.json first, then synthesized from CLI flags."""
    if args.manifest:
        candidates = [args.manifest]
    else:
        candidates = [os.path.join(input_dir, "workshop.json"),
                      os.path.join(input_dir, "manifest.json")]

    base = {}
    source = "synthesized from flags"
    for path in candidates:
        if os.path.isfile(path):
            with open(path, "r", encoding="utf-8") as fh:
                base = json.load(fh)  # raises on malformed JSON — surfaced to the caller
            source = os.path.basename(path)
            break

    default_name = (os.path.splitext(os.path.basename(args.output_o2r))[0]
                    if args.output_o2r else "soundfont-pack")
    manifest = {
        "name": args.name or base.get("name") or default_name,
        "version": args.version or base.get("version") or "0.1",
        "author": args.author or base.get("author") or "unknown",
        "game_version": args.game_version or base.get("game_version") or "us.rev0",
        # Stable pack identity (load order, disable list); defaults to the output filename stem.
        "id": args.id or base.get("id") or default_name,
        # What this pack carries, so the Workshop tab and future tooling can tell pack kinds apart.
        "pack_type": "soundfont",
    }
    for k, v in base.items():
        manifest.setdefault(k, v)
    validate_pack_id(manifest["id"])
    return json.dumps(manifest, indent=2, sort_keys=True).encode("utf-8"), source


def write_deterministic_o2r(output_o2r, entries):
    arc_entries = sorted(entries, key=lambda e: e[0])
    with zipfile.ZipFile(output_o2r, "w", zipfile.ZIP_DEFLATED) as z:
        for arc, data in arc_entries:
            info = zipfile.ZipInfo(arc, date_time=FIXED_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, data)


def cmd_pack(args):
    input_dir = args.input_dir
    if not os.path.isdir(input_dir):
        sys.stderr.write("error: input directory not found: %s\n" % input_dir)
        return 2

    blob, fontname, errors, warnings, reports = build_gft1(input_dir, args.predictors,
                                                           encode=True)
    for msg in reports:
        print("  %s" % msg)
    for msg in warnings:
        print("warn:  %s" % msg)
    for msg in errors:
        sys.stderr.write("error: %s\n" % msg)
    if errors:
        sys.stderr.write("error: %d problem(s) must be fixed before packing; nothing written\n"
                         % len(errors))
        return 1

    try:
        manifest_bytes, manifest_source = resolve_manifest_bytes(args, input_dir)
    except json.JSONDecodeError as exc:
        sys.stderr.write("error: pack metadata is not valid JSON (%s)\n" % exc)
        return 2
    except ValueError as exc:
        sys.stderr.write("error: %s\n" % exc)
        return 2

    # Metadata is stored as workshop.json: "manifest.json" is libultraship's reserved archive
    # manifest (numeric game_version schema) and our string game_version made LUS throw on mount.
    write_deterministic_o2r(args.output_o2r, [
        ("audio/font/" + fontname, blob),
        ("workshop.json", manifest_bytes),
    ])
    print("wrote %s (audio/font/%s, %d bytes, metadata %s)"
          % (args.output_o2r, fontname, len(blob), manifest_source))
    return 0


def check_dir(input_dir, npred):
    """Validate a project directory without encoding or writing. Returns process exit code."""
    _blob, fontname, errors, warnings, reports = build_gft1(input_dir, npred, encode=False)
    for msg in reports:
        print("  %s" % msg)
    for msg in warnings:
        print("warn:  %s" % msg)
    for msg in errors:
        sys.stderr.write("error: %s\n" % msg)
    print("check: %s" % input_dir)
    if errors:
        print("FAILED: %d problem(s)" % len(errors))
        return 1
    print("OK (%s)" % fontname)
    return 0


def check_pack(path):
    """Validate an existing .o2r soundfont pack without writing. Returns process exit code."""
    if not zipfile.is_zipfile(path):
        sys.stderr.write("error: %s is not a valid .o2r (zip) archive\n" % path)
        return 2
    errors = []
    font_count = 0
    with zipfile.ZipFile(path, "r") as z:
        names = z.namelist()
        if "workshop.json" not in names and "manifest.json" not in names:
            errors.append("no workshop.json (pack metadata) at archive root")
        for name in names:
            if name in ("workshop.json", "manifest.json"):
                continue
            if not name.startswith("audio/font/"):
                errors.append("%s: not under audio/font/ (unknown entry)" % name)
                continue
            fontname = name[len("audio/font/"):]
            if fontname not in FONT_NAME_SET:
                errors.append("%s: not a known font name" % name)
                continue
            blob = z.read(name)
            info, perr = parse_gft1(blob, check_gsmp=True)
            for msg in perr:
                errors.append("%s: %s" % (name, msg))
            if info is not None and not perr:
                if info["stockFontId"] != FONT_NAMES.index(fontname):
                    errors.append("%s: stockFontId %d does not match %s (id %d)"
                                  % (name, info["stockFontId"], fontname,
                                     FONT_NAMES.index(fontname)))
                else:
                    font_count += 1
                    print("  %s: %d instrument(s), %d envelope(s), %d sample(s), %d bytes"
                          % (name, info["numInstruments"], info["numEnvelopes"],
                             info["numSamples"], info["size"]))
        meta = None
        if "workshop.json" in names:
            try:
                meta = json.loads(z.read("workshop.json"))
            except Exception as exc:  # noqa: BLE001
                errors.append("workshop.json is not valid JSON (%s)" % exc)
        if isinstance(meta, dict):
            try:
                validate_pack_id(meta.get("id"))
            except ValueError as exc:
                errors.append("workshop.json: %s" % exc)
            ptype = meta.get("pack_type")
            if ptype is not None and ptype != "soundfont":
                errors.append('workshop.json "pack_type" is "%s" (want "soundfont")' % ptype)
    print("pack: %s" % path)
    print("  %d soundfont override(s)" % font_count)
    if font_count == 0:
        errors.append("no soundfont override entries found — pack overrides nothing")
    for msg in errors:
        sys.stderr.write("error: %s\n" % msg)
    if errors:
        print("FAILED: %d problem(s)" % len(errors))
        return 1
    print("OK")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Build, scaffold and validate soundfont overlay packs (GFT1 v1)",
        epilog="Examples:\n"
               "  gen_soundfont_pack.py init FONT_DDBGM_BIG_BLUE -o my-font/\n"
               "  gen_soundfont_pack.py pack my-font/ -o my-font.o2r --name \"My Font\"\n"
               "  gen_soundfont_pack.py check my-font/\n"
               "  gen_soundfont_pack.py check my-font.o2r",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)

    ap_init = sub.add_parser("init", help="scaffold an editable font project from the dump")
    ap_init.add_argument("font", nargs="?", default=None,
                         help="one of the 23 font names (see --names)")
    ap_init.add_argument("-o", "--output-dir", default=None,
                         help="project directory to write")
    ap_init.add_argument("--names", action="store_true",
                         help="list the 23 known font names and exit")
    ap_init.add_argument("--force", action="store_true",
                         help="overwrite an existing font.json in the output directory")
    ap_init.add_argument("--skip-samples", action="store_true",
                         help="do not re-encode dumped stock WAVs; emit silent placeholders for "
                              "every sample (fast scaffold; the full re-encode of large fonts "
                              "like FONT_SOUND_EFFECTS takes hours at ~2k frames/sec)")
    ap_init.add_argument("--predictors", type=int, default=2,
                         help="codebook predictors per sample, 1..8 (default 2, matching stock)")

    ap_pack = sub.add_parser("pack", help="validate a project and write a deterministic .o2r")
    ap_pack.add_argument("input_dir", help="font project directory (contains font.json)")
    ap_pack.add_argument("-o", "--output-o2r", required=True, help="output .o2r path")
    ap_pack.add_argument("--manifest", default=None,
                         help="pack metadata json (default <input>/workshop.json or manifest.json)")
    ap_pack.add_argument("--name", default=None, help="pack name for synthesized metadata")
    ap_pack.add_argument("--author", default=None, help="pack author for synthesized metadata")
    ap_pack.add_argument("--version", default=None, help="pack version for synthesized metadata")
    ap_pack.add_argument("--game-version", default=None, help="target game build (default us.rev0)")
    ap_pack.add_argument("--id", default=None,
                         help="stable pack identifier, e.g. author.packname (default: output "
                              "filename stem); commas are not allowed")
    ap_pack.add_argument("--predictors", type=int, default=2,
                         help="codebook predictors per encoded WAV, 1..8 (default 2)")

    ap_check = sub.add_parser("check", help="re-validate a project dir or an .o2r pack")
    ap_check.add_argument("input", help="font project directory or .o2r pack")
    ap_check.add_argument("--predictors", type=int, default=2, help=argparse.SUPPRESS)

    args = ap.parse_args()

    if args.command == "init":
        if args.names:
            for i, name in enumerate(FONT_NAMES):
                print("%2d %s" % (i, name))
            return 0
        if not 1 <= args.predictors <= 8:
            sys.stderr.write("error: --predictors must be 1..8\n")
            return 2
        return cmd_init(args)

    if args.command == "pack":
        if not 1 <= args.predictors <= 8:
            sys.stderr.write("error: --predictors must be 1..8\n")
            return 2
        return cmd_pack(args)

    # check
    target = args.input
    if os.path.isfile(target) and target.lower().endswith(".o2r"):
        return check_pack(target)
    if os.path.isdir(target):
        return check_dir(target, args.predictors)
    sys.stderr.write("error: check target is neither a directory nor an .o2r pack: %s\n" % target)
    return 2


if __name__ == "__main__":
    sys.exit(main())
