IBI conversion for CHF2DB annotation files (records chf225-chf229)
===================================================================

Source files: chf225.ecg, chf226.ecg, chf227.ecg, chf228.ecg, chf229.ecg
Sampling frequency: 128 samples/second

The .ecg files are WFDB beat-annotation files. Each beat annotation has a
sample timestamp (stored as intervals from the previous annotation). IBI is
calculated as:

    IBI_s  = (beat_sample[i] - beat_sample[i-1]) / 128
    IBI_ms = IBI_s * 1000

COLUMN LAYOUT  (IBI is always the SECOND column)
    chf2xx_IBI.csv:   beat_index, IBI_ms, beat_time_s, beat_time_samples,
                      beat_type_code, beat_symbol, IBI_s, heart_rate_bpm
    all_records_IBI.csv: record, IBI_ms, beat_index, beat_time_s,
                      beat_time_samples, beat_type_code, beat_symbol,
                      IBI_s, heart_rate_bpm

The .ibi files contain one IBI value per line, in milliseconds.
The first beat of each record has no IBI (empty cell) in the per-record CSVs.

No artifact/ectopic-beat correction was applied; all detected beat annotations
are retained, while signal-quality/noise annotations are not treated as beats.
Long gaps (up to several minutes) exist in some records; filter them before
statistics. plot_ibi_3d.py excludes IBIs outside 300-2000 ms by default.
