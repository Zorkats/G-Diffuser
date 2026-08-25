#!/usr/bin/env python3
"""Pack a directory of <key>.wav files into a deterministic .o2r sample pack (N64 VADPCM).

Quick start:

    python tools/gen_sample_pack.py my-sample-dir/ my-samples.o2r --name "My Samples" --author you

    # validate a source folder or an existing pack without writing anything:
    python tools/gen_sample_pack.py my-sample-dir/ --check
    python tools/gen_sample_pack.py my-samples.o2r --check

Input:
  <input_dir>/
    <key>.wav           PCM WAV (8/16-bit, mono or stereo — stereo is downmixed by averaging).
                        <key> is the dump sample key "<BANK>__0x<ADDR>__<SIZE>" exactly as
                        emitted by the audio dump (dump/audio/samples/*.wav, manifest.tsv,
                        fonts/*.json), e.g. "SAMPLE_SOUND_EFFECTS__0x1EB30__1440". Unknown keys
                        are warned about and skipped (see --names).
    <key>.loop.json     (optional) {"start": N, "end": M} in decoded-sample units. Absent means
                        one-shot (loop.count = 0, predictor state zeroed). When present:
                        start % 16 == 0 and start < end <= decodedLength are required. The packer
                        synthesizes the 16-s16 predictor state at the loop point by decoding its
                        own payload up to loop start (bit-exact with the runtime decoder, which
                        seeds A_LOOP history straight from this state — port/n64_audio_hle.c
                        RunAdpcm, "state[0..15] is the FULL last output frame in temporal
                        order"). loop.count is written as 0xFFFFFFFF ("loop forever"): every
                        looped stock sample in dump/audio/fonts/*.json uses that sentinel.
    workshop.json       (optional) pack metadata; synthesized from the --name/--author/
                        --version/--id flags when absent (same rules as gen_sequence_pack.py).

Output: one .o2r (ZIP) holding each sample at archive path "audio/sample/<key>" plus
workshop.json at the root (pack_type "sample").

Container ("GSMP", version 1, ALL little-endian, hand-packed with struct.pack):
  0  magic "GSMP" | 4 version u16=1 | 6 codec u16=0 (CODEC_ADPCM) | 8 reserved u32=0
  12 encodedSize u32 (payload bytes, % 9 == 0) | 16 decodedLength u32 (== encodedSize/9*16)
  20 loop.start u32 | 24 loop.end u32 | 28 loop.count u32
  32 loop.predictorState s16[16] (always written; meaningful iff count != 0)
  64 book.order u32 (1..2) | 68 book.npred u32 (1..8) | 72 book.coefs s16[8*order*npred]
  then crc32 u32 (zlib.crc32 of the payload), then the payload bytes.

Payload: CODEC_ADPCM frames, 9 bytes each — 1 header byte (shift<<4 | predictor index) + 8
bytes carrying 16 nibbles -> 16 s16 samples. Book layout coefs[pred*8*order + tap*8 + col]
(tap 0 = older history, tap 1 = newer history; the tap-1 row doubles as the residual
coefficients via the i-1-k indexing), matching decode_adpcm() below and
tools/gen_dump_all_audio.py / torch/src/gdx/dump_audio.cpp decodeAdpcm.

Pitch: if dump/audio/fonts/*.json (or dump/audio/manifest.tsv) is present, the stock per-sample
playbackRateHz (tuning x 32000) is the resample target. Without dump data the WAV's own rate is
kept — the runtime deliberately excludes tuning overrides, so pitch correctness then depends on
the modder matching the stock rate.

The encoder is stdlib-only: least-squares codebook training (normal equations solved with
in-house Gaussian elimination, deterministic quantile-seeded k-means refinement into
--predictors rows, order <= 2), then per-frame exhaustive search over predictors x shifts
0..12 simulating the exact decoder (structure of my_encodeframe in
torch/src/factories/naudio/v0/AIFCDecode.cpp:282). Every sample is self-verified by decoding
its own payload and reporting SNR dB (warning below 20 dB).

The archive is written deterministically (sorted entries, fixed 1980 timestamps) so identical
inputs produce byte-identical output.
"""
import argparse
import json
import math
import os
import re
import struct
import sys
import wave
import zipfile
import zlib

FIXED_DATE = (1980, 1, 1, 0, 0, 0)

GSMP_MAGIC = b"GSMP"
GSMP_VERSION = 1
GSMP_CODEC_ADPCM = 0
GSMP_HEADER_SIZE = 72  # everything before book.coefs
FRAME_SAMPLES = 16
FRAME_BYTES = 9
MAX_SHIFT = 12
LOOP_FOREVER = 0xFFFFFFFF  # stock looped samples all use this count (dump/audio/fonts/*.json)

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_DUMP_AUDIO = os.path.join(_REPO_ROOT, "dump", "audio")

KEY_RE = re.compile(r"^[A-Za-z0-9_]+__0x[0-9A-Fa-f]+__[0-9]+$")
KEY_SUFFIX = ".wav"
LOOP_SUFFIX = ".loop.json"


# ════════════════════════════════════════════════════════════════════════════════════════════════
# Dump-derived knowledge: accepted sample keys + stock per-sample playback rates.
# ════════════════════════════════════════════════════════════════════════════════════════════════
def load_dump_info():
    """Return (keys, rates) from dump/audio: keys = set of known sample keys, rates =
    {key: playbackRateHz}. Empty when the repo has no dump data."""
    keys = set()
    rates = {}
    fonts_dir = os.path.join(_DUMP_AUDIO, "fonts")
    if os.path.isdir(fonts_dir):
        for f in sorted(os.listdir(fonts_dir)):
            if not f.endswith(".json"):
                continue
            try:
                with open(os.path.join(fonts_dir, f), "r", encoding="utf-8") as fh:
                    doc = json.load(fh)
            except Exception:  # noqa: BLE001 -- a broken dump file must not kill packing
                continue
            for key, meta in (doc.get("samples") or {}).items():
                keys.add(key)
                rate = meta.get("playbackRateHz")
                if isinstance(rate, (int, float)) and rate > 0:
                    rates[key] = int(round(rate))
    manifest = os.path.join(_DUMP_AUDIO, "manifest.tsv")
    if os.path.isfile(manifest):
        with open(manifest, encoding="utf-8") as fh:
            for line in fh:
                parts = line.rstrip("\n").split("\t")
                if len(parts) < 5 or parts[0].startswith("#") or parts[2] == "sampleKey":
                    continue
                key = parts[2]
                keys.add(key)
                if key not in rates:
                    try:
                        rate = int(parts[4])
                        if rate > 0:
                            rates[key] = rate
                    except ValueError:
                        pass
    samples_dir = os.path.join(_DUMP_AUDIO, "samples")
    if os.path.isdir(samples_dir):
        for f in os.listdir(samples_dir):
            if f.endswith(KEY_SUFFIX):
                keys.add(f[:-len(KEY_SUFFIX)])
    return keys, rates


# ════════════════════════════════════════════════════════════════════════════════════════════════
# VADPCM decode — ported from tools/gen_dump_all_audio.py decode_adpcm (block-convolution path,
# == torch/src/gdx/dump_audio.cpp decodeAdpcm, == port/n64_audio_hle.c RunAdpcm). CODEC_ADPCM
# only (codec 0, 9-byte frames); the packer never emits SMALL_ADPCM.
# ════════════════════════════════════════════════════════════════════════════════════════════════
def _clamp_s16(v):
    if v > 32767:
        return 32767
    if v < -32768:
        return -32768
    return v


def decode_adpcm(data, order, npred, coefs):
    """Decode a whole CODEC_ADPCM payload. Returns list[int] of s16 samples. History seeds
    from 0 (A_INIT, start of a fresh sample)."""
    row_stride = 8 * order

    def bc(pred, tap, col):
        idx = pred * row_stride + tap * 8 + col
        return coefs[idx] if 0 <= idx < len(coefs) else 0

    out = []
    h1 = h2 = 0  # clamped history: newer(h1), older(h2)
    nframes = len(data) // FRAME_BYTES
    for f in range(nframes):
        base = f * FRAME_BYTES
        header = data[base]
        shift = (header >> 4) & 0xF
        pred = header & 0xF
        if pred >= npred:
            pred = 0  # defensive: out-of-range predictor row decodes as all-zero coefficients
        e_h2, e_h1 = h2, h1  # sub-block entry history (older, newer)
        for sub in range(2):
            e = []
            for i in range(8):
                si = sub * 8 + i
                bv = data[base + 1 + (si // 2)]
                nib = (bv >> 4) & 0xF if (si % 2 == 0) else bv & 0xF
                if nib & 0x8:
                    nib -= 16
                e.append(nib << shift)
            sblk = []
            for i in range(8):
                acc = bc(pred, 0, i) * e_h2 + bc(pred, 1, i) * e_h1
                for k in range(i):
                    acc += bc(pred, 1, i - 1 - k) * e[k]
                acc += e[i] << 11
                v = _clamp_s16(acc >> 11)
                sblk.append(v)
                out.append(v)
            e_h2, e_h1 = sblk[6], sblk[7]
        h2, h1 = e_h2, e_h1
    return out


# ════════════════════════════════════════════════════════════════════════════════════════════════
# Codebook design — least-squares predictor training (VADPCM tabledesign approach, stdlib only).
# ════════════════════════════════════════════════════════════════════════════════════════════════
def _solve(A, b):
    """Gaussian elimination with partial pivoting (Gauss-Jordan). Returns the solution vector,
    or None when the system is singular. Deterministic (fixed pivot rule)."""
    n = len(b)
    M = [list(A[i]) + [b[i]] for i in range(n)]
    for col in range(n):
        piv = col
        for r in range(col + 1, n):
            if abs(M[r][col]) > abs(M[piv][col]):
                piv = r
        if abs(M[piv][col]) < 1e-9:
            return None
        M[col], M[piv] = M[piv], M[col]
        for r in range(n):
            if r == col:
                continue
            fac = M[r][col] / M[col][col]
            if fac == 0.0:
                continue
            for c in range(col, n + 1):
                M[r][c] -= fac * M[col][c]
    return [M[i][n] / M[i][i] for i in range(n)]


def _accumulate_normal_eq(frames, order):
    """Normal equations for min sum (x[i] - a1*x[i-1] [- a2*x[i-2]])^2 over all frames.
    frames = [(h2, h1, [16 samples])] with h2/h1 the two preceding clean-input samples."""
    R = [[0.0] * order for _ in range(order)]
    r = [0.0] * order
    for h2, h1, fr in frames:
        for i in range(FRAME_SAMPLES):
            v = [h1 if i == 0 else fr[i - 1],
                 h2 if i < 2 else fr[i - 2]][:order]
            y = fr[i]
            for a in range(order):
                r[a] += y * v[a]
                for b_ in range(a, order):
                    R[a][b_] += v[a] * v[b_]
    for a in range(order):
        for b_ in range(a):
            R[a][b_] = R[b_][a]
    return R, r


def _train_predictor(frames, order):
    """Least-squares predictor coefficients (float, length == order). Zeros when singular."""
    R, r = _accumulate_normal_eq(frames, order)
    sol = _solve(R, r)
    return sol if sol is not None else [0.0] * order


def _frame_residuals(frames, coefs):
    """Open-loop per-frame residual vectors (16 floats each) under the given predictor."""
    a1 = coefs[0]
    a2 = coefs[1] if len(coefs) > 1 else 0.0
    out = []
    for h2, h1, fr in frames:
        res = []
        for i in range(FRAME_SAMPLES):
            x1 = h1 if i == 0 else fr[i - 1]
            x2 = h2 if i < 2 else fr[i - 2]
            res.append(fr[i] - (a1 * x1 + a2 * x2))
        out.append(res)
    return out


def _residual_sse(res):
    return sum(v * v for v in res)


def _kmeans_split(frames, residuals, npred, order, global_coefs):
    """Deterministic k-means over per-frame residual vectors: quantile split by residual
    energy for init, fixed 8 iterations, lowest-index tie-breaks. Returns npred coefficient
    vectors; degenerate clusters fall back to the global predictor."""
    n = len(frames)
    energies = sorted((_residual_sse(res), i) for i, res in enumerate(residuals))
    centroids = []
    for k in range(npred):
        lo = k * n // npred
        hi = (k + 1) * n // npred
        members = [residuals[i] for _e, i in energies[lo:hi]] or [residuals[energies[0][1]]]
        centroids.append([sum(m[j] for m in members) / len(members) for j in range(FRAME_SAMPLES)])

    assign = [0] * n
    for _it in range(8):
        changed = False
        for i, res in enumerate(residuals):
            best_k, best_d = 0, None
            for k, cen in enumerate(centroids):
                d = sum((res[j] - cen[j]) ** 2 for j in range(FRAME_SAMPLES))
                if best_d is None or d < best_d:
                    best_k, best_d = k, d
            if assign[i] != best_k:
                assign[i] = best_k
                changed = True
        if not changed:
            break
        for k in range(npred):
            members = [residuals[i] for i in range(n) if assign[i] == k]
            if members:
                centroids[k] = [sum(m[j] for m in members) / len(members)
                                for j in range(FRAME_SAMPLES)]

    # Two refine rounds against TRAINED predictors (assign by actual residual SSE, retrain).
    coefs = [global_coefs] * npred
    for _round in range(3):  # initial train + 2 refinement rounds
        coefs = []
        for k in range(npred):
            cluster = [frames[i] for i in range(n) if assign[i] == k]
            coefs.append(_train_predictor(cluster, order) if cluster else list(global_coefs))
        if _round == 2:
            break
        for i, (h2, h1, fr) in enumerate(frames):
            best_k, best_e = 0, None
            for k in range(npred):
                e = _residual_sse(_frame_residuals([(h2, h1, fr)], coefs[k])[0])
                if best_e is None or e < best_e:
                    best_k, best_e = k, e
            assign[i] = best_k
    return coefs


def _quantize_book_row(coefs, order):
    """Expand raw predictor coefficients into one 8*order book row (tap-major: f0[0..7] then
    f1[0..7], matching coefs[pred*8*order + tap*8 + col]). f0/f1 are the impulse responses of
    the quantized filter: out[i] = f0[i]*h2/2048 + f1[i]*h1/2048 + sum f1[i-1-k]*e[k]/2048."""
    qa = [max(-32768, min(32767, int(round(a * 2048.0)))) for a in coefs]
    a1 = qa[0] / 2048.0
    a2 = (qa[1] / 2048.0) if order > 1 else 0.0
    f0 = [0.0] * 8
    f1 = [0.0] * 8
    f0_m2, f0_m1 = 1.0, 0.0  # f0 tracks the coefficient of h2 = out[-2]
    f1_m2, f1_m1 = 0.0, 1.0  # f1 tracks the coefficient of h1 = out[-1]
    for i in range(8):
        f0[i] = a1 * f0_m1 + a2 * f0_m2
        f1[i] = a1 * f1_m1 + a2 * f1_m2
        f0_m2, f0_m1 = f0_m1, f0[i]
        f1_m2, f1_m1 = f1_m1, f1[i]
    row = f0 + f1
    return [max(-32768, min(32767, int(round(v)))) for v in row]


def design_book(samples, order, npred):
    """Train a codebook for the given s16 samples. Returns (coefs, order, npred) — coefs is
    8*order*npred s16 values. Order is dropped to 1 when it wins on residual energy."""
    padded = list(samples)
    while len(padded) % FRAME_SAMPLES:
        padded.append(0)
    frames = []
    h1 = h2 = 0
    for base in range(0, len(padded), FRAME_SAMPLES):
        fr = padded[base:base + FRAME_SAMPLES]
        frames.append((h2, h1, fr))
        h2, h1 = fr[-2], fr[-1]

    global2 = _train_predictor(frames, 2)
    global1 = _train_predictor(frames, 1)
    if order == 2:
        # Keep order 2 unless order 1 is strictly better (stock books are all order 2).
        e2 = sum(_residual_sse(r) for r in _frame_residuals(frames, global2))
        e1 = sum(_residual_sse(r) for r in _frame_residuals(frames, global1))
        if e1 < e2:
            order = 1
    global_coefs = (global2 if order == 2 else global1)

    if npred <= 1 or len(frames) < 2 * npred:
        rows = [global_coefs]
    else:
        rows = _kmeans_split(frames, _frame_residuals(frames, global_coefs), npred, order,
                             global_coefs)
    npred = len(rows)
    coefs = []
    for row in rows:
        coefs.extend(_quantize_book_row(row, order))
    return coefs, order, npred


# ════════════════════════════════════════════════════════════════════════════════════════════════
# Frame encode — exhaustive predictors x shifts, simulating the exact decoder per candidate
# (structure of my_encodeframe, torch/src/factories/naudio/v0/AIFCDecode.cpp:282: pick by
# squared error, carry the clamped 2-sample state across frames).
# ════════════════════════════════════════════════════════════════════════════════════════════════
def _qsample(x, shift):
    """x / 2^shift rounded to nearest, ties towards zero (qsample in AIFCDecode.cpp:152)."""
    if shift == 0:
        return x
    return (x + (1 << (shift - 1)) - (1 if x > 0 else 0)) >> shift


def _sim_frame(ins, h2, h1, f0, f1, shift):
    """Greedy-encode one frame with the given book row/shift, exactly as the decoder will
    reconstruct it (block convolution, per-sample clamping, clamped sub-block history).
    Returns (nibbles[16], decoded[16])."""
    nibs = []
    outs = []
    e_h2, e_h1 = h2, h1
    for sub in range(2):
        es = []
        sblk = []
        for i in range(8):
            idx = sub * 8 + i
            acc = f0[i] * e_h2 + f1[i] * e_h1
            for k in range(i):
                acc += f1[i - 1 - k] * es[k]
            predv = acc >> 11
            q = _qsample(ins[idx] - predv, shift)
            if q > 7:
                q = 7
            elif q < -8:
                q = -8
            ev = q << shift
            v = _clamp_s16(predv + ev)
            nibs.append(q & 0xF)
            sblk.append(v)
            outs.append(v)
            es.append(ev)
        e_h2, e_h1 = sblk[6], sblk[7]
    return nibs, outs


def encode_adpcm(samples, order, npred, coefs):
    """Encode s16 samples to a CODEC_ADPCM payload. Returns (payload, decoded) where decoded
    is the exact decoder output of the payload (used for SNR self-check and loop state)."""
    row_stride = 8 * order
    f_rows = [(coefs[p * row_stride:p * row_stride + 8],
               coefs[p * row_stride + 8:p * row_stride + 16]) for p in range(npred)]

    padded = list(samples)
    while len(padded) % FRAME_SAMPLES:
        padded.append(0)

    payload = bytearray()
    decoded = []
    h1 = h2 = 0
    for base in range(0, len(padded), FRAME_SAMPLES):
        ins = padded[base:base + FRAME_SAMPLES]
        best = None
        for p in range(npred):
            f0, f1 = f_rows[p]
            for shift in range(MAX_SHIFT + 1):
                nibs, outs = _sim_frame(ins, h2, h1, f0, f1, shift)
                sse = 0
                for i in range(FRAME_SAMPLES):
                    d = outs[i] - ins[i]
                    sse += d * d
                if best is None or sse < best[0]:
                    best = (sse, p, shift, nibs, outs)
        _sse, bp, bshift, bnibs, bouts = best
        payload.append((bshift << 4) | (bp & 0xF))
        for i in range(0, FRAME_SAMPLES, 2):
            payload.append((bnibs[i] << 4) | bnibs[i + 1])
        decoded.extend(bouts)
        h2, h1 = bouts[14], bouts[15]
    return bytes(payload), decoded


def snr_db(reference, decoded):
    """SNR in dB of decoded vs reference (equal lengths). inf when the reference is silent."""
    sig = 0.0
    err = 0.0
    for x, y in zip(reference, decoded):
        sig += x * x
        d = x - y
        err += d * d
    if sig == 0.0:
        return float("inf")
    if err == 0.0:
        return float("inf")
    return 10.0 * math.log10(sig / err)


# ════════════════════════════════════════════════════════════════════════════════════════════════
# GSMP container (all little-endian, hand-packed).
# ════════════════════════════════════════════════════════════════════════════════════════════════
def pack_gsmp(payload, decoded_length, loop, order, npred, coefs):
    """loop = (start, end, count, state16)."""
    start, end, count, state = loop
    head = bytearray()
    head += GSMP_MAGIC
    head += struct.pack("<HHI", GSMP_VERSION, GSMP_CODEC_ADPCM, 0)
    head += struct.pack("<II", len(payload), decoded_length)
    head += struct.pack("<III", start, end, count)
    head += struct.pack("<16h", *state)
    head += struct.pack("<II", order, npred)
    head += struct.pack("<%dh" % len(coefs), *coefs)
    head += struct.pack("<I", zlib.crc32(payload) & 0xFFFFFFFF)
    return bytes(head) + payload


def parse_gsmp(blob):
    """Parse and validate a GSMP blob. Returns (info, errors); info is None on structural
    failure. Every failure class gets a distinct message."""
    errors = []
    if len(blob) < GSMP_HEADER_SIZE + 4:
        return None, ["entry too small for GSMP header (%d bytes)" % len(blob)]
    if blob[:4] != GSMP_MAGIC:
        return None, ["bad magic %r (want GSMP)" % blob[:4]]
    version, codec, reserved = struct.unpack_from("<HHI", blob, 4)
    encoded_size, decoded_length = struct.unpack_from("<II", blob, 12)
    lstart, lend, lcount = struct.unpack_from("<III", blob, 20)
    state = list(struct.unpack_from("<16h", blob, 32))
    order, npred = struct.unpack_from("<II", blob, 64)

    if version != GSMP_VERSION:
        errors.append("unsupported version %d (want %d)" % (version, GSMP_VERSION))
    if codec != GSMP_CODEC_ADPCM:
        errors.append("unsupported codec %d (want 0 = CODEC_ADPCM)" % codec)
    if reserved != 0:
        errors.append("reserved field must be 0, got %d" % reserved)
    if encoded_size % FRAME_BYTES != 0:
        errors.append("encodedSize %d is not a multiple of %d" % (encoded_size, FRAME_BYTES))
    elif decoded_length != encoded_size // FRAME_BYTES * FRAME_SAMPLES:
        errors.append("decodedLength %d != encodedSize/9*16 (%d)"
                      % (decoded_length, encoded_size // FRAME_BYTES * FRAME_SAMPLES))
    if not 1 <= order <= 2:
        errors.append("book.order %d out of range (1..2)" % order)
    if not 1 <= npred <= 8:
        errors.append("book.npred %d out of range (1..8)" % npred)

    if errors:
        return None, errors

    coef_count = 8 * order * npred
    coef_end = GSMP_HEADER_SIZE + 2 * coef_count
    expected = coef_end + 4 + encoded_size
    if len(blob) != expected:
        errors.append("entry size %d != expected %d (header+book+crc32+encodedSize)"
                      % (len(blob), expected))
        return None, errors

    coefs = list(struct.unpack_from("<%dh" % coef_count, blob, GSMP_HEADER_SIZE))
    stored_crc = struct.unpack_from("<I", blob, coef_end)[0]
    payload = blob[coef_end + 4:]

    if lcount != 0:
        if lstart % FRAME_SAMPLES != 0:
            errors.append("loop.start %d is not frame-aligned (%% 16 != 0)" % lstart)
        if not (lstart < lend <= decoded_length):
            errors.append("loop bounds invalid: start %d, end %d, decodedLength %d "
                          "(need start < end <= decodedLength)" % (lstart, lend, decoded_length))
    elif lstart != 0 or lend != 0:
        errors.append("one-shot entry (count 0) must carry loop start/end 0, got %d/%d"
                      % (lstart, lend))

    actual_crc = zlib.crc32(payload) & 0xFFFFFFFF
    if stored_crc != actual_crc:
        errors.append("crc32 mismatch: stored 0x%08X, computed 0x%08X" % (stored_crc, actual_crc))

    if not errors:
        for f in range(encoded_size // FRAME_BYTES):
            pred = blob[coef_end + 4 + f * FRAME_BYTES] & 0xF
            if pred >= npred:
                errors.append("frame %d uses predictor %d >= book.npred %d" % (f, pred, npred))
                break

    info = {
        "version": version, "codec": codec, "encodedSize": encoded_size,
        "decodedLength": decoded_length, "loop": (lstart, lend, lcount), "state": state,
        "order": order, "npred": npred, "coefs": coefs, "crc32": stored_crc,
        "payload": payload,
    }
    return info, errors


# ════════════════════════════════════════════════════════════════════════════════════════════════
# WAV loading / resampling (stdlib wave only; 8/16-bit PCM, mono/stereo).
# ════════════════════════════════════════════════════════════════════════════════════════════════
def load_wav(path):
    """Return (samples, rate) — mono s16 samples. 8-bit PCM is unsigned; stereo is downmixed
    by averaging. Raises ValueError with a clear message on anything else."""
    try:
        with wave.open(path, "rb") as w:
            channels = w.getnchannels()
            width = w.getsampwidth()
            rate = w.getframerate()
            nframes = w.getnframes()
            raw = w.readframes(nframes)
            comptype = w.getcomptype()
    except wave.Error as exc:
        raise ValueError("not a readable PCM WAV: %s" % exc)
    if comptype != "NONE":
        raise ValueError("compressed WAV (%s) is not supported — use PCM" % comptype)
    if channels not in (1, 2):
        raise ValueError("%d channels not supported (mono/stereo only)" % channels)
    if width == 1:
        vals = [b - 128 for b in raw]
        vals = [v << 8 for v in vals]
    elif width == 2:
        n = len(raw) // 2
        vals = list(struct.unpack("<%dh" % n, raw[:n * 2]))
    else:
        raise ValueError("%d-bit samples not supported (8/16-bit PCM only)" % (width * 8))
    if channels == 2:
        vals = [(vals[i] + vals[i + 1]) // 2 for i in range(0, len(vals) - 1, 2)]
    return vals, rate


def resample_linear(samples, src_rate, dst_rate):
    """Linear-interpolation resample. Deterministic (pure float math in index order)."""
    if src_rate == dst_rate or not samples:
        return list(samples)
    out_len = max(1, int(round(len(samples) * dst_rate / src_rate)))
    ratio = src_rate / dst_rate
    out = []
    last = len(samples) - 1
    for j in range(out_len):
        pos = j * ratio
        i = int(pos)
        if i >= last:
            out.append(samples[last])
            continue
        frac = pos - i
        out.append(int(round(samples[i] * (1.0 - frac) + samples[i + 1] * frac)))
    return out


# ════════════════════════════════════════════════════════════════════════════════════════════════
# Pack planning / building.
# ════════════════════════════════════════════════════════════════════════════════════════════════
def validate_pack_id(pid):
    """Pack ids join into comma-separated lists at runtime (load order, disable list), so an id
    must be a non-empty string with no commas. Raises ValueError with a clear message."""
    if not isinstance(pid, str) or not pid.strip():
        raise ValueError('pack metadata "id" must be a non-empty string')
    if "," in pid:
        raise ValueError('pack id "%s" must not contain commas (the runtime stores ids in '
                         'comma-joined lists)' % pid)


def resolve_manifest_bytes(args):
    """Return (manifest_bytes, source_description). Prefer an on-disk metadata file
    (workshop.json, then legacy manifest.json); otherwise synthesize one from the CLI flags so a
    missing metadata file is never a fatal error."""
    if args.manifest:
        candidates = [args.manifest]
    else:
        candidates = [os.path.join(args.input_dir, "workshop.json"),
                      os.path.join(args.input_dir, "manifest.json")]

    base = {}
    source = "synthesized from flags"
    for path in candidates:
        if os.path.isfile(path):
            with open(path, "r", encoding="utf-8") as fh:
                base = json.load(fh)  # raises on malformed JSON — surfaced to the caller
            source = os.path.basename(path)
            break

    default_name = (os.path.splitext(os.path.basename(args.output_o2r))[0]
                    if args.output_o2r else "sample-pack")
    manifest = {
        "name": args.name or base.get("name") or default_name,
        "version": args.version or base.get("version") or "0.1",
        "author": args.author or base.get("author") or "unknown",
        "game_version": args.game_version or base.get("game_version") or "us.rev0",
        # Stable pack identity (load order, disable list); defaults to the output filename stem,
        # which is also the runtime's fallback for old packs with no id.
        "id": args.id or base.get("id") or default_name,
        # What this pack carries, so the Workshop tab and future tooling can tell pack kinds apart.
        "pack_type": "sample",
    }
    # Preserve any extra fields the modder added to their metadata file (depends, conflicts, ...).
    for k, v in base.items():
        manifest.setdefault(k, v)
    validate_pack_id(manifest["id"])
    return json.dumps(manifest, indent=2, sort_keys=True).encode("utf-8"), source


def collect_wavs(input_dir):
    """All <key>.wav files directly inside input_dir (not recursive — sample keys are flat)."""
    entries = []
    for f in sorted(os.listdir(input_dir)):
        full = os.path.join(input_dir, f)
        if os.path.isfile(full) and f.lower().endswith(KEY_SUFFIX):
            entries.append((f[:-len(KEY_SUFFIX)], full))
    return entries


def read_loop_sidecar(input_dir, key):
    """Return (start, end) from <key>.loop.json, or None when absent. Raises ValueError on
    malformed sidecars."""
    path = os.path.join(input_dir, key + LOOP_SUFFIX)
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as fh:
            doc = json.load(fh)
    except json.JSONDecodeError as exc:
        raise ValueError("%s: loop sidecar is not valid JSON (%s)" % (key + LOOP_SUFFIX, exc))
    if not isinstance(doc, dict) or not isinstance(doc.get("start"), int) \
            or not isinstance(doc.get("end"), int):
        raise ValueError('%s: loop sidecar must be {"start": int, "end": int} '
                         '(decoded-sample units)' % (key + LOOP_SUFFIX))
    return doc["start"], doc["end"]


def encode_one(key, samples, rate, rates, npred, loop_req):
    """Full pipeline for one sample. Returns (blob, report_lines, warnings)."""
    warnings = []
    target = rates.get(key)
    if target is not None and target != rate:
        samples = resample_linear(samples, rate, target)
        rate_note = "resampled %d -> %d Hz (stock rate)" % (rate, target)
    elif target is not None:
        rate_note = "rate %d Hz (matches stock)" % rate
    else:
        rate_note = ("rate %d Hz kept (no stock rate known — pitch depends on the modder "
                     "matching the stock tuning)" % rate)
        warnings.append("%s: no stock playbackRateHz found; keeping source rate" % key)

    coefs, order, npred_out = design_book(samples, 2, npred)
    payload, decoded = encode_adpcm(samples, order, npred_out, coefs)
    decoded_length = len(decoded)

    if loop_req is not None:
        start, end = loop_req
        if start % FRAME_SAMPLES != 0:
            raise ValueError("%s: loop.start %d is not frame-aligned (%% 16 != 0)" % (key, start))
        if not (0 <= start < end <= decoded_length):
            raise ValueError("%s: loop bounds invalid: start %d, end %d, decodedLength %d "
                             "(need 0 <= start < end <= decodedLength)"
                             % (key, start, end, decoded_length))
        # Predictor state = the full last output frame before the loop point (temporal order,
        # state[15] newest) — exactly what the runtime seeds A_LOOP history from.
        state = [0] * (FRAME_SAMPLES - start) + decoded[max(0, start - FRAME_SAMPLES):start] \
            if start < FRAME_SAMPLES else decoded[start - FRAME_SAMPLES:start]
        loop = (start, end, LOOP_FOREVER, state)
    else:
        loop = (0, 0, 0, [0] * FRAME_SAMPLES)

    padded_ref = samples + [0] * (decoded_length - len(samples))
    snr = snr_db(padded_ref, decoded)
    blob = pack_gsmp(payload, decoded_length, loop, order, npred_out, coefs)
    report = ("%s: %d frames, order %d, %d predictor(s), %s, SNR %s dB"
              % (key, decoded_length // FRAME_SAMPLES, order, npred_out, rate_note,
                 "inf" if snr == float("inf") else "%.1f" % snr))
    if snr < 20.0:
        warnings.append("%s: SNR %.1f dB is below 20 dB — the encode is audibly lossy" %
                        (key, snr))
    return blob, report, warnings


def plan_pack(input_dir, rates, known_keys, npred, check_only=False):
    """Validate and (unless check_only) encode every <key>.wav in input_dir. Returns
    (entries, errors, warnings, reports). Collects ALL problems."""
    entries = []
    errors = []
    warnings = []
    reports = []

    wavs = collect_wavs(input_dir)
    if not wavs:
        errors.append("no .wav files found in %s" % input_dir)
        return entries, errors, warnings, reports

    seen = set()
    for key, full in wavs:
        if key in seen:
            warnings.append("%s: duplicate (case-insensitive .wav match) - skipped" % key)
            continue
        seen.add(key)
        if not KEY_RE.match(key):
            warnings.append('%s: not a valid sample key ("<BANK>__0x<ADDR>__<SIZE>") - skipped'
                            % key)
            continue
        if known_keys and key not in known_keys:
            warnings.append("%s: not a known sample key (see --names) - skipped" % key)
            continue
        try:
            loop_req = read_loop_sidecar(input_dir, key)
        except ValueError as exc:
            errors.append(str(exc))
            continue
        try:
            samples, rate = load_wav(full)
        except ValueError as exc:
            errors.append("%s: %s" % (os.path.basename(full), exc))
            continue
        if not samples:
            errors.append("%s: empty WAV (0 frames)" % key)
            continue
        if check_only:
            resampled = resample_linear(samples, rate, rates.get(key, rate))
            decoded_length = -(-len(resampled) // FRAME_SAMPLES) * FRAME_SAMPLES
            if loop_req is not None:
                start, end = loop_req
                if start % FRAME_SAMPLES != 0:
                    errors.append("%s: loop.start %d is not frame-aligned (%% 16 != 0)"
                                  % (key, start))
                elif not (0 <= start < end <= decoded_length):
                    errors.append("%s: loop bounds invalid: start %d, end %d, decodedLength %d"
                                  % (key, start, end, decoded_length))
            reports.append("%s: %d samples @ %d Hz -> %d decoded samples%s"
                           % (key, len(samples), rate, decoded_length,
                              ", loop %d..%d" % loop_req if loop_req else ", one-shot"))
            continue
        try:
            blob, report, warns = encode_one(key, samples, rate, rates, npred, loop_req)
        except ValueError as exc:
            errors.append(str(exc))
            continue
        warnings.extend(warns)
        reports.append(report)
        entries.append(("audio/sample/" + key, blob))

    missing = sorted(known_keys - seen) if known_keys else []
    if known_keys and missing:
        print("info:  %d known sample(s) have no .wav in this pack (left as stock)"
              % len(missing))
    return entries, errors, warnings, reports


def check_pack(path, known_keys):
    """Validate an existing .o2r pack without writing. Returns process exit code."""
    if not zipfile.is_zipfile(path):
        sys.stderr.write("error: %s is not a valid .o2r (zip) archive\n" % path)
        return 2
    errors = []
    sample_count = 0
    with zipfile.ZipFile(path, "r") as z:
        names = z.namelist()
        if "workshop.json" not in names and "manifest.json" not in names:
            errors.append("no workshop.json (pack metadata) at archive root")
        for name in names:
            if name in ("workshop.json", "manifest.json"):
                continue
            if not name.startswith("audio/sample/"):
                errors.append("%s: not under audio/sample/ (unknown entry)" % name)
                continue
            key = name[len("audio/sample/"):]
            if not KEY_RE.match(key):
                errors.append('%s: not a valid sample key ("<BANK>__0x<ADDR>__<SIZE>")' % name)
                continue
            if known_keys and key not in known_keys:
                errors.append("%s: not a known sample key (see --names)" % name)
                continue
            info, gerr = parse_gsmp(z.read(name))
            for msg in gerr:
                errors.append("%s: %s" % (name, msg))
            if info is not None and not gerr:
                sample_count += 1
        meta = None
        if "workshop.json" in names:
            try:
                meta = json.loads(z.read("workshop.json"))
            except Exception as exc:  # noqa: BLE001
                errors.append("workshop.json is not valid JSON (%s)" % exc)
        if isinstance(meta, dict):
            pid = meta.get("id")
            if not isinstance(pid, str) or not pid.strip():
                errors.append('workshop.json has no usable "id" (old packs fall back to the '
                              'archive basename; add an "id" like "author.packname")')
            elif "," in pid:
                errors.append('workshop.json "id" "%s" must not contain commas (ids are stored '
                              'in comma-joined lists)' % pid)
            ptype = meta.get("pack_type")
            if ptype is not None and ptype != "sample":
                errors.append('workshop.json "pack_type" is "%s" (want "sample")' % ptype)
    print("pack: %s" % path)
    print("  %d sample override(s)" % sample_count)
    if sample_count == 0:
        errors.append("no sample override entries found — pack overrides nothing")
    for msg in errors:
        sys.stderr.write("error: %s\n" % msg)
    if errors:
        print("FAILED: %d problem(s)" % len(errors))
        return 1
    print("OK")
    return 0


def check_dir(input_dir, rates, known_keys, npred):
    """Validate a pack source directory without writing. Returns process exit code."""
    entries, errors, warnings, reports = plan_pack(input_dir, rates, known_keys, npred,
                                                   check_only=True)
    for msg in reports:
        print("  %s" % msg)
    for msg in warnings:
        print("warn:  %s" % msg)
    for msg in errors:
        sys.stderr.write("error: %s\n" % msg)
    print("check: %s" % input_dir)
    print("  %d sample(s) would be packed, %d skipped, %d error(s)"
          % (len(reports), len(warnings), len(errors)))
    if errors:
        print("FAILED")
        return 1
    print("OK")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Pack <key>.wav samples into a deterministic .o2r sample pack (N64 VADPCM)",
        epilog="Examples:\n"
               "  gen_sample_pack.py samples/ my-samples.o2r --name \"My Samples\" --author you\n"
               "  gen_sample_pack.py samples/ --check\n"
               "  gen_sample_pack.py my-samples.o2r --check",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?", default=None,
                    help="pack source directory (or an .o2r pack when --check)")
    ap.add_argument("output_o2r", nargs="?", default=None,
                    help="output .o2r path (omit with --check)")
    ap.add_argument("--check", action="store_true",
                    help="validate the input directory or an existing .o2r pack; write nothing")
    ap.add_argument("--names", action="store_true",
                    help="list the known sample keys (from dump/audio) and exit")
    ap.add_argument("--manifest", default=None,
                    help="pack metadata json (default <input>/workshop.json or manifest.json)")
    ap.add_argument("--name", default=None, help="pack name for synthesized metadata")
    ap.add_argument("--author", default=None, help="pack author for synthesized metadata")
    ap.add_argument("--version", default=None, help="pack version for synthesized metadata")
    ap.add_argument("--game-version", default=None, help="target game build (default us.rev0)")
    ap.add_argument("--id", default=None,
                    help="stable pack identifier for synthesized metadata, e.g. author.packname "
                         "(default: output filename stem); commas are not allowed")
    ap.add_argument("--predictors", type=int, default=2,
                    help="codebook predictors per sample, 1..8 (default 2, matching stock)")
    args = ap.parse_args()

    known_keys, rates = load_dump_info()

    if args.names:
        if not known_keys:
            print("no dump data found under dump/audio — any key of the form "
                  '"<BANK>__0x<ADDR>__<SIZE>" is accepted')
            return 0
        for i, key in enumerate(sorted(known_keys)):
            rate = rates.get(key)
            print("%3d %-45s %s" % (i, key, "%d Hz" % rate if rate else "rate unknown"))
        return 0

    if not 1 <= args.predictors <= 8:
        sys.stderr.write("error: --predictors must be 1..8\n")
        return 2
    if args.input is None:
        sys.stderr.write("error: input is required\n")
        return 2

    # --check on an existing pack file.
    if args.check and os.path.isfile(args.input) and args.input.lower().endswith(".o2r"):
        return check_pack(args.input, known_keys)

    # From here on the input is treated as a directory. Keep args.input_dir compatible with helpers.
    args.input_dir = args.input

    if args.check:
        if not os.path.isdir(args.input_dir):
            sys.stderr.write("error: input directory not found: %s\n" % args.input_dir)
            return 2
        return check_dir(args.input_dir, rates, known_keys, args.predictors)

    if not args.output_o2r:
        sys.stderr.write("error: output_o2r is required (or pass --check to validate only)\n")
        return 2
    if not os.path.isdir(args.input_dir):
        sys.stderr.write("error: input directory not found: %s\n" % args.input_dir)
        return 2

    try:
        manifest_bytes, manifest_source = resolve_manifest_bytes(args)
    except json.JSONDecodeError as exc:
        sys.stderr.write("error: pack metadata is not valid JSON (%s)\n" % exc)
        return 2
    except ValueError as exc:
        sys.stderr.write("error: %s\n" % exc)
        return 2

    entries, errors, warnings, reports = plan_pack(args.input_dir, rates, known_keys,
                                                   args.predictors)
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
    if not entries:
        sys.stderr.write("error: nothing to pack (every .wav was skipped - see warnings above)\n")
        return 2

    # Metadata is stored as workshop.json: "manifest.json" is libultraship's reserved archive
    # manifest (numeric game_version schema) and our string game_version made LUS throw on mount.
    arc_entries = list(entries)
    arc_entries.append(("workshop.json", manifest_bytes))
    arc_entries.sort(key=lambda e: e[0])

    with zipfile.ZipFile(args.output_o2r, "w", zipfile.ZIP_DEFLATED) as z:
        for arc, data in arc_entries:
            info = zipfile.ZipInfo(arc, date_time=FIXED_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, data)

    print("wrote %s (%d sample override(s), metadata %s)"
          % (args.output_o2r, len(entries), manifest_source))
    for msg in warnings:
        print("  skipped/warned: %s" % msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
