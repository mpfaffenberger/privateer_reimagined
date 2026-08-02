"""Dependency-free duration measurement for generated MP3 voice assets."""

from __future__ import annotations

from pathlib import Path

_MPEG1_LAYER3_KBPS = (0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192,
                      224, 256, 320, 0)
_MPEG2_LAYER3_KBPS = (0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128,
                      144, 160, 0)


def _id3_payload_size(header: bytes) -> int:
    if len(header) < 10 or header[:3] != b"ID3":
        return 0
    if any(value & 0x80 for value in header[6:10]):
        raise ValueError("invalid ID3 synchsafe size")
    size = ((header[6] << 21) | (header[7] << 14) |
            (header[8] << 7) | header[9])
    return 10 + size


def mp3_duration_seconds(path: Path) -> float:
    """Return duration by walking MPEG Layer III frames.

    Generated voice files are ordinary CBR MP3s, but walking frames also works
    for VBR output and avoids optional ffmpeg/mutagen dependencies.
    """
    data = path.read_bytes()
    offset = _id3_payload_size(data[:10])
    duration = 0.0
    frames = 0

    while offset + 4 <= len(data):
        header = int.from_bytes(data[offset:offset + 4], "big")
        if ((header >> 21) & 0x7FF) != 0x7FF:
            offset += 1
            continue

        version = (header >> 19) & 0x3
        layer = (header >> 17) & 0x3
        bitrate_index = (header >> 12) & 0xF
        sample_index = (header >> 10) & 0x3
        padding = (header >> 9) & 0x1
        if version == 1 or layer != 1 or bitrate_index in (0, 15) or sample_index == 3:
            offset += 1
            continue

        if version == 3:  # MPEG-1
            sample_rate = (44100, 48000, 32000)[sample_index]
            bitrate = _MPEG1_LAYER3_KBPS[bitrate_index]
            samples = 1152
            frame_length = (144000 * bitrate) // sample_rate + padding
        else:  # MPEG-2 / MPEG-2.5
            rates = ((22050, 24000, 16000) if version == 2 else
                     (11025, 12000, 8000))
            sample_rate = rates[sample_index]
            bitrate = _MPEG2_LAYER3_KBPS[bitrate_index]
            samples = 576
            frame_length = (72000 * bitrate) // sample_rate + padding

        if frame_length < 4 or offset + frame_length > len(data):
            break
        duration += samples / sample_rate
        frames += 1
        offset += frame_length

    if frames == 0:
        raise ValueError(f"no MPEG Layer III frames found in {path}")
    return duration
