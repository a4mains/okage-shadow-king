#!/usr/bin/env python3
"""Extract nominally paired or capture-boundary PCM segments without fitting.

The outputs retain each source's sample rate. This tool does not resample,
cross-correlate, compare waveforms, or assert audio equivalence. Frame-count
checks establish nominal duration only. This tool does not inspect vblank
indexes in nominal mode, so sample-accurate temporal alignment remains unproven.
Boundary mode extracts the offsets declared by original/native capture indexes;
different segment extents remain different and do not imply temporal alignment.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path
import struct
import wave

def _require(condition, message):
    if not condition:
        raise ValueError(message)

def _sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()

def _read_json(path, label):
    try:
        value = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f'cannot read {label} {path}: {error}') from error
    _require(isinstance(value, dict), f'{label} must be a JSON object')
    return value

def _native_sidecars(pcm_path):
    audio_manifest = pcm_path.with_suffix('.json')
    run_manifest = None
    suffix = '.png.audio.pcm'
    if pcm_path.name.endswith(suffix):
        prefix = pcm_path.with_name(pcm_path.name[:-len(suffix)])
        run_manifest = Path(str(prefix) + '.run.json')
    return audio_manifest, run_manifest

def _native_capture_frame_index(pcm_path):
    suffix = '.png.audio.pcm'
    if not pcm_path.name.endswith(suffix):
        return None
    prefix = pcm_path.name[:-len(suffix)]
    candidate = pcm_path.with_name(prefix + '.png.frames') / 'manifest.json'
    return candidate if candidate.is_file() else None

def _run_frame_count(run, label, *, positional_frames=False):
    _require(run.get('exit_code') == 0, f'{label} run manifest is not a successful run')
    command = run.get('command')
    _require(isinstance(command, list) and all(isinstance(x, str) for x in command),
             f'{label} run manifest has no command list')
    if '--frames' in command:
        index = command.index('--frames') + 1
        _require(index < len(command), f'{label} --frames option has no value')
    else:
        _require(positional_frames and len(command) >= 3,
                 f'{label} run command has no --frames option')
        index = 2
    try:
        frames = int(command[index])
    except (TypeError, ValueError) as error:
        raise ValueError(f'{label} frame-count value is invalid') from error
    _require(frames > 0, f'{label} --frames must be positive')
    return frames

def _validate_optional_format_fields(manifest):
    """Check format declarations if a future/adjacent manifest includes them."""
    for key in ('channels', 'channel_count'):
        if key in manifest:
            _require(manifest[key] == 2, f'native audio manifest {key} must be 2')
    for key in ('bits_per_sample', 'sample_width_bits'):
        if key in manifest:
            _require(manifest[key] == 16, f'native audio manifest {key} must be 16')
    if 'sample_width_bytes' in manifest:
        _require(manifest['sample_width_bytes'] == 2,
                 'native audio manifest sample_width_bytes must be 2')
    for key in ('format', 'encoding', 'sample_format'):
        if key in manifest:
            value = str(manifest[key]).lower().replace('-', '_').replace(' ', '_')
            _require(value in ('pcm_s16le', 'signed_16_bit_pcm', 's16le', 'pcm'),
                     f'unsupported native audio manifest {key}: {manifest[key]}')

def _stats(pcm_bytes):
    _require(len(pcm_bytes) % 4 == 0, 'stereo 16-bit PCM byte length is not divisible by 4')
    count = len(pcm_bytes) // 4
    _require(count > 0, 'PCM segment is empty')
    sums = [0, 0]
    peaks = [0, 0]
    nonzero = [0, 0]
    for left, right in struct.iter_unpack('<hh', pcm_bytes):
        for channel, sample in enumerate((left, right)):
            magnitude = abs(sample)
            sums[channel] += sample * sample
            peaks[channel] = max(peaks[channel], magnitude)
            nonzero[channel] += sample != 0
    labels = ('left', 'right')
    return {
        labels[channel]: {
            'peak_abs': peaks[channel],
            'rms': math.sqrt(sums[channel] / count),
            'nonzero_samples': nonzero[channel],
            'sample_frames': count,
        }
        for channel in range(2)
    }

def _write_wav_exclusive(path, pcm_bytes, rate):
    with path.open('xb') as output:
        with wave.open(output, 'wb') as wav:
            wav.setnchannels(2)
            wav.setsampwidth(2)
            wav.setframerate(rate)
            wav.writeframes(pcm_bytes)

def _read_stereo_wav(path, *, require_rate_multiple_of_60=False):
    try:
        with wave.open(str(path), 'rb') as source:
            _require(source.getcomptype() == 'NONE', 'original WAV must be uncompressed PCM')
            _require(source.getnchannels() == 2, 'original WAV must be stereo')
            _require(source.getsampwidth() == 2, 'original WAV must use 16-bit samples')
            rate = source.getframerate()
            _require(rate > 0, 'original WAV sample rate must be positive')
            if require_rate_multiple_of_60:
                _require(rate % 60 == 0, 'original WAV rate must be a positive multiple of 60 Hz')
            frames = source.getnframes()
            data = source.readframes(frames)
    except (wave.Error, EOFError) as error:
        raise ValueError(f'invalid original WAV: {error}') from error
    _require(len(data) == frames * 4, 'original WAV data extent is truncated')
    return rate, frames, data

def _parse_nonnegative_int(value, label):
    _require(isinstance(value, int) and not isinstance(value, bool) and value >= 0,
             f'{label} must be a nonnegative integer')
    return value

def _validate_original_audio_index(index, wav_frames, wav_rate, run_frames=None):
    _require(index.get('sample_rate') == wav_rate,
             'original audio index sample_rate disagrees with WAV')
    _require(index.get('channels') == 2, 'original audio index must declare 2 channels')
    total = _parse_nonnegative_int(index.get('total_sample_frames'),
                                   'original audio index total_sample_frames')
    _require(total == wav_frames,
             f'original audio index has {total} frames but WAV has {wav_frames}')
    _require(isinstance(index.get('boundary'), str) and 'OnNewFrame' in index['boundary'],
             'original audio index lacks the documented OnNewFrame boundary')
    scope = index.get('scope')
    _require(isinstance(scope, str) and 'block buffered' in scope.lower(),
             'original audio index lacks the documented block-buffering scope')
    rows = index.get('vblanks')
    _require(isinstance(rows, list) and rows, 'original audio index has no vblank boundary rows')
    offsets = {}
    previous_vblank = 0
    previous_offset = -1
    for row in rows:
        _require(isinstance(row, dict), 'original audio index row must be an object')
        vblank = _parse_nonnegative_int(row.get('vblank_start'), 'original vblank_start')
        offset = _parse_nonnegative_int(row.get('sample_frame_offset'),
                                        'original sample_frame_offset')
        _require(vblank == previous_vblank + 1,
                 'original audio index vblank rows must be strictly consecutive from 1')
        _require(offset >= previous_offset,
                 'original audio index sample offsets must be nondecreasing')
        _require(offset <= total, 'original audio index offset exceeds total_sample_frames')
        if run_frames is not None:
            _require(vblank <= run_frames,
                     'original audio index vblank exceeds successful run frame count')
        offsets[vblank] = offset
        previous_vblank, previous_offset = vblank, offset
    if run_frames is not None:
        _require(len(offsets) == run_frames,
                 'original audio index boundary count disagrees with run frame count')
    return offsets, total

def _validate_native_frame_index(index, pcm_frames, run_frames=None):
    boundary = index.get('boundary')
    _require(isinstance(boundary, str) and 'completed native Session::tick' in boundary,
             'native frame index lacks its documented tick boundary')
    if 'complete' in index:
        _require(index['complete'] is True, 'native frame index is incomplete')
    captures = index.get('captures')
    _require(isinstance(captures, list) and captures,
             'native frame index has no capture boundary rows')
    offsets = {}
    previous_tick = 0
    previous_offset = -1
    for capture in captures:
        _require(isinstance(capture, dict), 'native frame index row must be an object')
        tick = _parse_nonnegative_int(capture.get('tick'), 'native capture tick')
        offset = _parse_nonnegative_int(capture.get('audio_sample_frame_offset'),
                                        'native audio_sample_frame_offset')
        _require(tick > previous_tick,
                 'native frame index ticks must be strictly increasing')
        _require(offset >= previous_offset,
                 'native frame index sample offsets must be nondecreasing')
        _require(offset <= pcm_frames,
                 'native frame index offset exceeds native PCM extent')
        if run_frames is not None:
            _require(tick <= run_frames,
                     'native frame index tick exceeds successful run frame count')
        offsets[tick] = offset
        previous_tick, previous_offset = tick, offset
    return offsets

def _validate_output_prefix(output_prefix):
    original_output = Path(str(output_prefix) + '.original.wav')
    native_output = Path(str(output_prefix) + '.native.wav')
    report_output = Path(str(output_prefix) + '.json')
    for path in (original_output, native_output, report_output):
        _require(not path.exists() and not path.is_symlink(),
                 f'refusing existing output: {path}')
    return original_output, native_output, report_output

def extract_route_audio_boundaries(original_wav, original_audio_index,
                                   original_start_vblank, original_end_boundary,
                                   native_pcm, native_rate, output_prefix, *,
                                   native_frame_index=None,
                                   native_start_tick=None, native_end_tick=None,
                                   native_first_tick=None, ticks=None):
    """Extract source-indexed audio segments while retaining unequal extents.

    The original end boundary may be a recorded vblank integer or the explicit
    post-stop `total_sample_frames` endpoint, spelled `stop`. Native extraction
    uses a supplied capture manifest's tick offsets when available. Otherwise,
    callers must explicitly supply the fixed-tick range (`native_first_tick`,
    `ticks`); that fallback is nominal sample-rate arithmetic, not an index.
    """
    original_wav = Path(original_wav)
    original_audio_index = Path(original_audio_index)
    native_pcm = Path(native_pcm)
    output_prefix = Path(output_prefix)
    _require(original_wav.is_file(), f'original WAV not found: {original_wav}')
    _require(original_audio_index.is_file(),
             f'original audio index not found: {original_audio_index}')
    _require(native_pcm.is_file(), f'native PCM not found: {native_pcm}')
    _require(output_prefix.parent.is_dir(),
             'output prefix parent directory must already exist')
    original_output, native_output, report_output = _validate_output_prefix(output_prefix)

    original_rate, original_total_frames, original_bytes = _read_stereo_wav(original_wav)
    original_index = _read_json(original_audio_index, 'original audio index')
    original_run_path = original_wav.with_suffix('.run.json')
    original_run = None
    original_run_frames = None
    source_manifests = {}
    if original_run_path.is_file():
        original_run = _read_json(original_run_path, 'original run manifest')
        original_run_frames = _run_frame_count(original_run, 'original', positional_frames=True)
        command = original_run['command']
        if '--audio' in command:
            audio_arg = command.index('--audio') + 1
            _require(audio_arg < len(command), 'original --audio option has no path')
            _require(Path(command[audio_arg]).resolve() == original_wav.resolve(),
                     'original run manifest --audio path does not name the input WAV')
        source_manifests['original_run'] = {
            'path': str(original_run_path), 'sha256': _sha256(original_run_path),
            'exit_code': original_run['exit_code'], 'run_vblanks': original_run_frames,
        }
    original_offsets, original_index_total = _validate_original_audio_index(
        original_index, original_total_frames, original_rate, original_run_frames)
    original_start_vblank = _parse_nonnegative_int(original_start_vblank,
                                                   'original start vblank')
    _require(original_start_vblank in original_offsets,
             'requested original start vblank is not recorded in audio index')
    original_start = original_offsets[original_start_vblank]
    if isinstance(original_end_boundary, str) and original_end_boundary.lower() == 'stop':
        original_end = original_index_total
        original_end_label = 'post_stop_total_sample_frames'
    else:
        original_end_vblank = _parse_nonnegative_int(original_end_boundary,
                                                     'original end vblank')
        _require(original_end_vblank in original_offsets,
                 'requested original end vblank is not recorded in audio index')
        _require(original_end_vblank > original_start_vblank,
                 'original end vblank must follow start vblank')
        original_end = original_offsets[original_end_vblank]
        original_end_label = f'vblank_start_{original_end_vblank}'
    _require(original_end > original_start,
             'selected original audio boundaries contain no sample frames')
    original_segment = original_bytes[original_start * 4:original_end * 4]
    _require(len(original_segment) == (original_end - original_start) * 4,
             'selected original audio segment is truncated')

    _require(native_rate > 0 and native_rate % 60 == 0,
             'native-rate must be a positive multiple of 60 Hz')
    native_size = native_pcm.stat().st_size
    _require(native_size % 4 == 0, 'native PCM must be stereo interleaved signed 16-bit data')
    native_total_frames = native_size // 4
    audio_manifest_path, run_manifest_path = _native_sidecars(native_pcm)
    native_manifest = None
    native_run = None
    native_run_frames = None
    if audio_manifest_path.is_file():
        native_manifest = _read_json(audio_manifest_path, 'native audio manifest')
        _validate_optional_format_fields(native_manifest)
        _require(native_manifest.get('rate') == native_rate,
                 'native-rate disagrees with native audio manifest rate')
        _require(native_manifest.get('frames') == native_total_frames,
                 'native PCM extent disagrees with native audio manifest frames')
        source_manifests['native_audio'] = {
            'path': str(audio_manifest_path), 'sha256': _sha256(audio_manifest_path),
        }
    if run_manifest_path and run_manifest_path.is_file():
        native_run = _read_json(run_manifest_path, 'native run manifest')
        native_run_frames = _run_frame_count(native_run, 'native')
        _require(native_total_frames == native_run_frames * (native_rate // 60),
                 'native PCM extent disagrees with run frame count at 60 Hz')
        source_manifests['native_run'] = {
            'path': str(run_manifest_path), 'sha256': _sha256(run_manifest_path),
            'exit_code': native_run['exit_code'], 'run_ticks': native_run_frames,
        }

    if native_frame_index is None:
        native_frame_index = _native_capture_frame_index(native_pcm)
    native_start_label = native_end_label = None
    if native_frame_index is not None:
        native_frame_index = Path(native_frame_index)
        native_index = _read_json(native_frame_index, 'native capture frame index')
        native_offsets = _validate_native_frame_index(
            native_index, native_total_frames, native_run_frames)
        _require(native_start_tick is not None and native_end_tick is not None,
                 'native index extraction requires --native-start-tick and --native-end-tick')
        native_start_tick = _parse_nonnegative_int(native_start_tick, 'native start tick')
        native_end_tick = _parse_nonnegative_int(native_end_tick, 'native end tick')
        _require(native_end_tick > native_start_tick,
                 'native end tick must follow start tick')
        _require(native_start_tick in native_offsets and native_end_tick in native_offsets,
                 'requested native start/end tick is not recorded in frame index')
        native_start = native_offsets[native_start_tick]
        native_end = native_offsets[native_end_tick]
        native_start_label = f'after_completed_tick_{native_start_tick}'
        native_end_label = f'after_completed_tick_{native_end_tick}'
        source_manifests['native_frame_index'] = {
            'path': str(native_frame_index), 'sha256': _sha256(native_frame_index),
            'boundary': native_index['boundary'],
        }
        native_boundary_scope = 'recorded_native_capture_boundaries'
    else:
        _require(native_first_tick is not None and ticks is not None,
                 'without a native frame index, declare --native-first-tick and --ticks')
        _require(native_first_tick > 0 and ticks > 0,
                 'native-first-tick and ticks must be positive')
        native_start_tick = native_first_tick
        native_end_tick = native_first_tick + ticks - 1
        if native_run_frames is not None:
            _require(native_end_tick <= native_run_frames,
                     'requested native tick interval exceeds successful run frame count')
        native_samples_per_tick = native_rate // 60
        native_start = (native_first_tick - 1) * native_samples_per_tick
        native_end = native_start + ticks * native_samples_per_tick
        native_start_label = f'fixed_tick_interval_start_{native_first_tick}'
        native_end_label = f'fixed_tick_interval_end_{native_end_tick}'
        native_boundary_scope = 'explicit_fixed_tick_sample_rate_arithmetic'
    _require(native_end > native_start, 'selected native audio boundaries contain no sample frames')
    _require(native_end <= native_total_frames,
             f'native audio boundary ends at sample frame {native_end}; PCM has {native_total_frames}')
    with native_pcm.open('rb') as source:
        source.seek(native_start * 4)
        native_segment = source.read((native_end - native_start) * 4)
    _require(len(native_segment) == (native_end - native_start) * 4,
             'selected native audio segment is truncated')

    original_frames = len(original_segment) // 4
    native_frames = len(native_segment) // 4
    duration_mismatch = original_frames != native_frames
    status = ('captured_unpaired_duration_mismatch' if duration_mismatch
              else 'captured_segments_unaligned')
    _write_wav_exclusive(original_output, original_segment, original_rate)
    _write_wav_exclusive(native_output, native_segment, native_rate)
    report = {
        'schema_version': 1,
        'status': status,
        'scope': ('Source-boundary PCM extraction and per-channel descriptive statistics only. '
                  'No sample comparison, padding, trimming, resampling, lag fitting, or equivalence pass.'),
        'alignment': {
            'temporal_alignment_proven': False,
            'sample_to_vblank_alignment_proven': False,
            'waveform_alignment_performed': False,
            'resampling_performed': False,
            'padding_performed': False,
            'trimming_performed': False,
            'waveform_equivalence_asserted': False,
            'duration_mismatch': duration_mismatch,
            'duration_mismatch_sample_frames': native_frames - original_frames,
        },
        'original_boundary': {
            'index': str(original_audio_index), 'index_sha256': _sha256(original_audio_index),
            'index_boundary': original_index['boundary'],
            'start_vblank': original_start_vblank,
            'start_sample_frame': original_start,
            'end_boundary': original_end_label,
            'end_sample_frame_exclusive': original_end,
            'total_sample_frames': original_index_total,
            'post_stop_endpoint_used': original_end_label == 'post_stop_total_sample_frames',
        },
        'native_boundary': {
            'scope': native_boundary_scope,
            'frame_index_boundary': (native_index['boundary']
                                     if native_frame_index is not None else None),
            'start_tick': native_start_tick, 'start_label': native_start_label,
            'start_sample_frame': native_start,
            'end_tick': native_end_tick, 'end_label': native_end_label,
            'end_sample_frame_exclusive': native_end,
            'total_sample_frames': native_total_frames,
        },
        'sources': {
            'original_wav': {
                'path': str(original_wav), 'sha256': _sha256(original_wav),
                'sample_rate_hz': original_rate, 'total_sample_frames': original_total_frames,
                'segment_frames': original_frames, 'statistics': _stats(original_segment),
            },
            'native_pcm': {
                'path': str(native_pcm), 'sha256': _sha256(native_pcm),
                'sample_rate_hz': native_rate, 'total_sample_frames': native_total_frames,
                'segment_frames': native_frames, 'statistics': _stats(native_segment),
            },
            'manifests': source_manifests,
        },
        'segments': {
            'original': {
                'path': str(original_output), 'sample_rate_hz': original_rate,
                'sample_frames': original_frames, 'sha256': _sha256(original_output),
                'statistics': _stats(original_segment),
            },
            'native': {
                'path': str(native_output), 'sample_rate_hz': native_rate,
                'sample_frames': native_frames, 'sha256': _sha256(native_output),
                'statistics': _stats(native_segment),
            },
        },
    }
    with report_output.open('x') as output:
        output.write(json.dumps(report, indent=2) + '\n')
    return report

def compare_route_audio(original_wav, native_pcm, native_rate, native_first_tick,
                        ticks, output_prefix):
    original_wav = Path(original_wav)
    native_pcm = Path(native_pcm)
    output_prefix = Path(output_prefix)
    _require(ticks > 0, 'ticks must be positive')
    _require(native_first_tick > 0, 'native-first-tick must be positive')
    _require(native_rate > 0 and native_rate % 60 == 0,
             'native-rate must be a positive multiple of 60 Hz')
    _require(original_wav.is_file(), f'original WAV not found: {original_wav}')
    _require(native_pcm.is_file(), f'native PCM not found: {native_pcm}')
    _require(output_prefix.parent.is_dir(), 'output prefix parent directory must already exist')

    original_output = Path(str(output_prefix) + '.original.wav')
    native_output = Path(str(output_prefix) + '.native.wav')
    report_output = Path(str(output_prefix) + '.json')
    for path in (original_output, native_output, report_output):
        _require(not path.exists() and not path.is_symlink(), f'refusing existing output: {path}')

    try:
        with wave.open(str(original_wav), 'rb') as source:
            _require(source.getcomptype() == 'NONE', 'original WAV must be uncompressed PCM')
            _require(source.getnchannels() == 2, 'original WAV must be stereo')
            _require(source.getsampwidth() == 2, 'original WAV must use 16-bit samples')
            original_rate = source.getframerate()
            _require(original_rate > 0 and original_rate % 60 == 0,
                     'original WAV rate must be a positive multiple of 60 Hz')
            original_frames = source.getnframes()
            expected_original_frames = ticks * (original_rate // 60)
            _require(original_frames == expected_original_frames,
                     f'original WAV has {original_frames} stereo frames; expected '
                     f'{expected_original_frames} for {ticks} nominal 60 Hz ticks')
            original_bytes = source.readframes(original_frames)
    except (wave.Error, EOFError) as error:
        raise ValueError(f'invalid original WAV: {error}') from error
    _require(len(original_bytes) == original_frames * 4, 'original WAV data extent is truncated')

    native_samples_per_tick = native_rate // 60
    native_first_tick = int(native_first_tick)
    native_last_tick = native_first_tick + ticks - 1
    native_start_frame = (native_first_tick - 1) * native_samples_per_tick
    native_end_frame = native_start_frame + ticks * native_samples_per_tick
    native_size = native_pcm.stat().st_size
    _require(native_size % 4 == 0, 'native PCM must be stereo interleaved signed 16-bit data')
    native_total_frames = native_size // 4

    audio_manifest_path, run_manifest_path = _native_sidecars(native_pcm)
    native_manifest = None
    native_run = None
    native_run_frames = None
    source_manifests = {}
    if audio_manifest_path.is_file():
        native_manifest = _read_json(audio_manifest_path, 'native audio manifest')
        _validate_optional_format_fields(native_manifest)
        _require(native_manifest.get('rate') == native_rate,
                 'native-rate disagrees with native audio manifest rate')
        _require(native_manifest.get('frames') == native_total_frames,
                 'native PCM extent disagrees with native audio manifest frames')
        source_manifests['native_audio'] = {
            'path': str(audio_manifest_path), 'sha256': _sha256(audio_manifest_path),
        }
    if run_manifest_path and run_manifest_path.is_file():
        native_run = _read_json(run_manifest_path, 'native run manifest')
        native_run_frames = _run_frame_count(native_run, 'native')
        _require(native_last_tick <= native_run_frames,
                 'requested native tick interval exceeds successful run frame count')
        expected_run_samples = native_run_frames * native_samples_per_tick
        _require(native_total_frames == expected_run_samples,
                 'native PCM extent disagrees with run frame count at 60 Hz')
        source_manifests['native_run'] = {
            'path': str(run_manifest_path), 'sha256': _sha256(run_manifest_path),
            'exit_code': native_run['exit_code'], 'run_ticks': native_run_frames,
        }
    _require(native_end_frame <= native_total_frames,
             f'native PCM ends at sample frame {native_total_frames}; requested slice ends '
             f'at {native_end_frame}')
    with native_pcm.open('rb') as source:
        source.seek(native_start_frame * 4)
        native_bytes = source.read((native_end_frame - native_start_frame) * 4)
    _require(len(native_bytes) == (native_end_frame - native_start_frame) * 4,
             'native PCM slice is truncated')

    original_run_path = original_wav.with_suffix('.run.json')
    original_run = None
    if original_run_path.is_file():
        original_run = _read_json(original_run_path, 'original run manifest')
        original_run_frames = _run_frame_count(original_run, 'original', positional_frames=True)
        _require(original_run_frames == ticks,
                 'original WAV run frame count disagrees with requested tick count')
        command = original_run['command']
        if '--audio' in command:
            audio_index = command.index('--audio')
            _require(audio_index + 1 < len(command), 'original --audio option has no path')
            _require(Path(command[audio_index + 1]).resolve() == original_wav.resolve(),
                     'original run manifest --audio path does not name the input WAV')
        source_manifests['original_run'] = {
            'path': str(original_run_path), 'sha256': _sha256(original_run_path),
            'exit_code': original_run['exit_code'], 'run_ticks': original_run_frames,
        }

    _write_wav_exclusive(original_output, original_bytes, original_rate)
    _write_wav_exclusive(native_output, native_bytes, native_rate)
    report = {
        'schema_version': 1,
        'scope': ('Fixed-tick PCM extraction and descriptive per-channel statistics only. '
                  'No resampling, sample-lag fitting, waveform comparison, or audio-equivalence pass.'),
        'alignment': {
            'declared_native_first_tick': native_first_tick,
            'declared_native_last_tick': native_last_tick,
            'tick_count': ticks,
            'nominal_tick_rate_hz': 60,
            'native_start_sample_frame_zero_based': native_start_frame,
            'native_end_sample_frame_exclusive': native_end_frame,
            'native_sample_frames_per_tick': native_samples_per_tick,
            'original_start_sample_frame_zero_based': 0,
            'original_sample_frames_per_tick_nominal': original_rate // 60,
            'original_sample_count_matches_nominal_duration': True,
            'sample_to_vblank_alignment_proven': False,
            'waveform_alignment_performed': False,
            'resampling_performed': False,
            'waveform_equivalence_asserted': False,
            'note': ('This extraction does not inspect per-vblank sample indexes. Matching nominal '
                     'duration and declared route anchors do not prove exact sample-origin alignment.'),
        },
        'sources': {
            'original_wav': {
                'path': str(original_wav), 'sha256': _sha256(original_wav),
                'format': 'PCM signed 16-bit little-endian stereo WAV',
                'sample_rate_hz': original_rate, 'sample_frames': original_frames,
                'statistics': _stats(original_bytes),
            },
            'native_pcm': {
                'path': str(native_pcm), 'sha256': _sha256(native_pcm),
                'format': 'raw signed 16-bit little-endian interleaved stereo PCM',
                'sample_rate_hz': native_rate, 'sample_frames': native_total_frames,
                'statistics': _stats(native_bytes),
            },
            'manifests': source_manifests,
        },
        'segments': {
            'original': {
                'path': str(original_output), 'sample_rate_hz': original_rate,
                'sample_frames': original_frames, 'sha256': _sha256(original_output),
                'statistics': _stats(original_bytes),
            },
            'native': {
                'path': str(native_output), 'sample_rate_hz': native_rate,
                'sample_frames': native_end_frame - native_start_frame,
                'sha256': _sha256(native_output), 'statistics': _stats(native_bytes),
            },
        },
    }
    with report_output.open('x') as output:
        output.write(json.dumps(report, indent=2) + '\n')
    return report

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original-wav', type=Path, required=True,
                        help='original RecordedAudio stereo 16-bit PCM WAV')
    parser.add_argument('--native-pcm', type=Path, required=True,
                        help='native raw stereo signed 16-bit PCM capture')
    parser.add_argument('--native-rate', type=int, required=True,
                        help='native PCM sample rate in Hz, verified against .audio.json when present')
    parser.add_argument('--native-first-tick', type=int,
                        help='first native simulation tick included in the extracted segment')
    parser.add_argument('--ticks', type=int,
                        help='nominal interval length in 60 Hz simulation ticks')
    parser.add_argument('--output', type=Path, required=True,
                        help='new output prefix; writes .original.wav, .native.wav, and .json')
    parser.add_argument('--original-audio-index', type=Path,
                        help='switch to recorded-boundary extraction using this original .audio-index.json')
    parser.add_argument('--original-start-vblank', type=int,
                        help='recorded original vblank_start boundary for boundary mode')
    parser.add_argument('--original-end-boundary',
                        help='recorded original vblank_start number or "stop" for documented total_sample_frames')
    parser.add_argument('--native-frame-index', type=Path,
                        help='native selected-frame manifest with audio_sample_frame_offset entries; auto-discovered beside standard .png.audio.pcm captures')
    parser.add_argument('--native-start-tick', type=int,
                        help='start tick recorded in --native-frame-index')
    parser.add_argument('--native-end-tick', type=int,
                        help='end tick recorded in --native-frame-index')
    args = parser.parse_args()
    try:
        if args.original_audio_index is not None:
            _require(args.original_start_vblank is not None and
                     args.original_end_boundary is not None,
                     'boundary mode requires --original-start-vblank and --original-end-boundary')
            end_boundary = args.original_end_boundary
            if end_boundary.lower() != 'stop':
                try:
                    end_boundary = int(end_boundary)
                except ValueError as error:
                    raise ValueError('original end boundary must be a vblank number or stop') from error
            report = extract_route_audio_boundaries(
                args.original_wav, args.original_audio_index,
                args.original_start_vblank, end_boundary,
                args.native_pcm, args.native_rate, args.output,
                native_frame_index=args.native_frame_index,
                native_start_tick=args.native_start_tick,
                native_end_tick=args.native_end_tick,
                native_first_tick=args.native_first_tick, ticks=args.ticks)
        else:
            _require(args.native_first_tick is not None and args.ticks is not None,
                     'nominal mode requires --native-first-tick and --ticks')
            report = compare_route_audio(args.original_wav, args.native_pcm, args.native_rate,
                                         args.native_first_tick, args.ticks, args.output)
    except (OSError, ValueError, wave.Error) as error:
        parser.error(str(error))
    print(json.dumps({
        'status': report.get('status', 'nominal_fixed_tick_extraction'),
        'original_sample_frames': report['segments']['original']['sample_frames'],
        'native_sample_frames': report['segments']['native']['sample_frames'],
        'original_rate_hz': report['segments']['original']['sample_rate_hz'],
        'native_rate_hz': report['segments']['native']['sample_rate_hz'],
        'sample_to_vblank_alignment_proven': False,
        'waveform_equivalence_asserted': False,
    }))

if __name__ == '__main__':
    main()
