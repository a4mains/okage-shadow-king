#!/usr/bin/env python3
"""Extract nominally paired original/native PCM intervals without fitting.

The outputs retain each source's sample rate. This tool does not resample,
cross-correlate, compare waveforms, or assert audio equivalence. Frame-count
checks establish nominal duration only. This tool does not inspect vblank
indexes, so sample-accurate temporal alignment remains unproven.
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
    parser.add_argument('--native-first-tick', type=int, required=True,
                        help='first native simulation tick included in the extracted segment')
    parser.add_argument('--ticks', type=int, required=True,
                        help='nominal interval length in 60 Hz simulation ticks')
    parser.add_argument('--output', type=Path, required=True,
                        help='new output prefix; writes .original.wav, .native.wav, and .json')
    args = parser.parse_args()
    try:
        report = compare_route_audio(args.original_wav, args.native_pcm, args.native_rate,
                                     args.native_first_tick, args.ticks, args.output)
    except (OSError, ValueError, wave.Error) as error:
        parser.error(str(error))
    print(json.dumps({
        'original_sample_frames': report['segments']['original']['sample_frames'],
        'native_sample_frames': report['segments']['native']['sample_frames'],
        'original_rate_hz': report['segments']['original']['sample_rate_hz'],
        'native_rate_hz': report['segments']['native']['sample_rate_hz'],
        'sample_to_vblank_alignment_proven': False,
        'waveform_equivalence_asserted': False,
    }))

if __name__ == '__main__':
    main()
