"""Convert PhysioNet CHF2DB .ecg beat-annotation files to IBI CSV files.

Output layout (IBI in the SECOND column):
    beat_index, IBI_ms, beat_time_s, beat_time_samples, beat_type_code,
    beat_symbol, IBI_s, heart_rate_bpm
"""
import csv
import struct
import sys
from pathlib import Path

FS = 128  # CHF2DB sampling frequency (Hz)

SYMBOLS = {1: "N", 2: "L", 3: "R", 4: "a", 5: "V", 6: "F", 7: "J", 8: "A",
           9: "S", 10: "E", 11: "j", 12: "/", 13: "Q", 25: "B", 34: "e",
           35: "n", 38: "f", 41: "r"}
BEAT_CODES = set(SYMBOLS)           # non-beat annotations (noise, rhythm...) are skipped
SKIP, NUM, SUB, CHN, AUX = 59, 60, 61, 62, 63


def read_annotations(path):
    """Return [(sample, code)] from an MIT-format annotation file."""
    data = Path(path).read_bytes()
    pos, t, out = 0, 0, []
    while pos + 2 <= len(data):
        w = struct.unpack_from("<H", data, pos)[0]
        pos += 2
        code, delta = w >> 10, w & 0x3FF
        if code == 0 and delta == 0:          # end of file
            break
        if code == SKIP:                      # 32-bit time increment follows
            hi, lo = struct.unpack_from("<HH", data, pos)
            pos += 4
            inc = (hi << 16) | lo
            if inc & 0x80000000:
                inc -= 1 << 32
            t += inc
            continue
        if code in (NUM, SUB, CHN):           # modifiers, no time change
            continue
        if code == AUX:                       # aux string of `delta` bytes
            pos += delta + (delta & 1)
            continue
        t += delta
        out.append((t, code))
    return out


def convert(ecg_path, out_dir):
    rec = Path(ecg_path).stem.lower()
    beats = [(s, c) for s, c in read_annotations(ecg_path) if c in BEAT_CODES]
    rows, prev = [], None
    for i, (s, c) in enumerate(beats, 1):
        if prev is None:
            ibi_s = ibi_ms = hr = ""
        else:
            ibi_s = (s - prev) / FS
            ibi_ms = ibi_s * 1000
            hr = 60 / ibi_s if ibi_s > 0 else ""
        rows.append([i, f"{ibi_ms:.6f}" if ibi_ms != "" else "",
                     f"{s / FS:.6f}", s, c, SYMBOLS[c],
                     f"{ibi_s:.6f}" if ibi_s != "" else "",
                     f"{hr:.6f}" if hr != "" else ""])
        prev = s
    header = ["beat_index", "IBI_ms", "beat_time_s", "beat_time_samples",
              "beat_type_code", "beat_symbol", "IBI_s", "heart_rate_bpm"]
    with open(Path(out_dir) / f"{rec}_IBI.csv", "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n"); w.writerow(header); w.writerows(rows)
    with open(Path(out_dir) / f"{rec}.ibi", "w") as f:
        for r in rows[1:]:
            f.write(f"{float(r[1]):.3f}\n")
    return rec, header, rows


if __name__ == "__main__":
    for p in sys.argv[2:]:
        rec, _, rows = convert(p, sys.argv[1])
        print(rec, len(rows), "beats")
