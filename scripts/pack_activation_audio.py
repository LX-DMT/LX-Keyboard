"""Pack the offline Huihui PCM WAV resources used by keyboard prompts."""
from pathlib import Path
import struct
import wave

folder = Path(__file__).resolve().parents[1] / 'firmware/keyboard/main/activation_audio'
data = bytearray()
entries = []
for index in range(11):
    with wave.open(str(folder / f'{index}.wav'), 'rb') as wav:
        assert (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) == (1, 2, 16000)
        pcm = wav.readframes(wav.getnframes())
    samples = struct.unpack(f'<{len(pcm)//2}h', pcm)
    audible = [i for i, value in enumerate(samples) if abs(value) > 60]
    assert audible, f'Empty voice clip: {index}'
    start, end = max(0, audible[0] - 640), min(len(samples), audible[-1] + 1 + 640)
    entries.append((len(data), end-start))
    data.extend(pcm[2*start:2*end])
(folder / 'clips.bin').write_bytes(data)
(folder / 'clips.h').write_text(
    '/* Offline Windows Huihui zh-CN, 16 kHz mono PCM16 LE. */\n'
    'static const struct { unsigned offset,samples; } activation_clips[]={\n'
    + ''.join(f'{{{offset},{count}}},\n' for offset, count in entries) + '};\n', encoding='utf8')
print(f'Packed {len(entries)} activation clips, {len(data)} bytes')
