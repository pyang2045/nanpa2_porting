"""Voice/jingle WAV loader check: synthetic files (mono, stereo, 11 kHz, extra
chunks before data) plus, if present, one real file of each format in the game
folder. Usage: wavcheck.py NANPA2 GAME_DIR SCRATCH_DIR"""
import glob, os, struct, subprocess, sys

exe, game, tmp = sys.argv[1:4]
os.makedirs(tmp, exist_ok=True)


def wav(path, ch, rate, n, pre=()):
    data = bytes((i * 7) & 255 for i in range(n * ch))
    fmt = struct.pack('<HHIIHH', 1, ch, rate, rate * ch, ch, 8)
    body = b'WAVE' + b'fmt ' + struct.pack('<I', 16) + fmt
    for cid, payload in pre:
        body += cid + struct.pack('<I', len(payload)) + payload + (b'\0' if len(payload) & 1 else b'')
    body += b'data' + struct.pack('<I', len(data)) + data + b'LIST' + struct.pack('<I', 4) + b'INFO'
    open(path, 'wb').write(b'RIFF' + struct.pack('<I', len(body)) + body)
    return path


files = [
    wav(f'{tmp}/mono22.wav', 1, 22050, 22050),
    wav(f'{tmp}/stereo22.wav', 2, 22050, 33075),
    wav(f'{tmp}/mono11.wav', 1, 11025, 11025),
    wav(f'{tmp}/extra.wav', 1, 22050, 11025, [(b'fact', b'\0\0\0\0'), (b'junk', b'abc')]),
    wav(f'{tmp}/extra_st.wav', 2, 22050, 11025, [(b'LIST', b'INFOxx')]),
]
files += sorted(glob.glob(f'{game}/SPEECH/I/I10955.WAV') + glob.glob(f'{game}/SPEECH/I/I10457.WAV')
                + glob.glob(f'{game}/SPEECH/K/K10286.WAV') + glob.glob(f'{game}/MUSIC/50.wav'))
r = subprocess.run([exe, '--wav-check'] + files, capture_output=True, text=True)
print(r.stdout, r.stderr)
sys.exit(r.returncode)
