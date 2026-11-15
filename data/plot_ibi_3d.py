import argparse
import datetime
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd

# 1. Configurable Project Root
PROJECT_ROOT = Path(__file__).parent.resolve()
OUTPUT_DIR = PROJECT_ROOT / "plots"

IBI_COLUMN = 2             # IBI is the SECOND column (1-based) in every converted CSV
DEFAULT_RANGE_MS = (300, 2000)  # physiological window used to exclude gaps / noise


def get_candidate_files(root_dir):
    """Recursively search for CSV files, prioritizing those with 'IBI' in the name."""
    all_csvs = sorted(root_dir.rglob("*.csv"))
    ibi_candidates = [f for f in all_csvs
                      if "IBI" in f.name.upper() and "SUMMARY" not in f.name.upper()]
    return ibi_candidates if ibi_candidates else all_csvs


def select_file(files):
    """Prompt user to select a file if multiple exist."""
    if not files:
        raise FileNotFoundError(f"No CSV files found recursively in {PROJECT_ROOT}.")
    if len(files) == 1:
        return files[0]

    print("Multiple candidate CSV files found:")
    for i, f in enumerate(files, 1):
        print(f"{i}. {f} ({f.stat().st_size / 1024:.1f} KB)")

    while True:
        try:
            choice = int(input("\nEnter the number of the file to process: "))
            if 1 <= choice <= len(files):
                return files[choice - 1]
            print("Invalid selection.")
        except ValueError:
            print("Please enter a valid number.")


def get_ibi_series(df, column=IBI_COLUMN):
    """Take the IBI column by POSITION (default: second column) and validate it.

    Returns a float array where invalid entries (empty / non-numeric / inf /
    non-positive) are NaN. Positions are kept so that beats on either side of a
    bad value are never joined together in the delay embedding.
    """
    if df.shape[1] < column:
        raise ValueError(f"CSV has only {df.shape[1]} columns; expected IBI in column {column}.")
    col_name = df.columns[column - 1]
    s = pd.to_numeric(df.iloc[:, column - 1], errors="coerce")

    print(f"\nUsing column {column}: '{col_name}'")
    print(f"- Total rows: {len(s)}")
    print(f"- Missing/Non-numeric: {s.isna().sum()}")
    print(f"- Zero values: {(s == 0).sum()}")
    print(f"- Negative values: {(s < 0).sum()}")
    print(f"- Infinite values: {np.isinf(s).sum()}")

    s = s.where(np.isfinite(s) & (s > 0))
    median_val = s.median()
    units = "unknown"
    if 0.2 < median_val < 2.0:
        units = "seconds"
    elif 200 < median_val < 2000:
        units = "milliseconds"
    print(f"- Inferred units: {units} (Median: {median_val:.3f})")
    return s.to_numpy(dtype=float), units, col_name


def apply_range(ibi, units, lo_ms, hi_ms):
    """Mark IBIs outside [lo_ms, hi_ms] as NaN (breaks the trajectory there)."""
    if lo_ms is None and hi_ms is None:
        return ibi
    scale = 1.0 if units == "milliseconds" else 0.001 if units == "seconds" else None
    if scale is None:
        print("Units unknown - range filter skipped.")
        return ibi
    out = ibi.copy()
    with np.errstate(invalid="ignore"):
        bad = np.zeros(len(out), dtype=bool)
        if lo_ms is not None:
            bad |= out < lo_ms * scale
        if hi_ms is not None:
            bad |= out > hi_ms * scale
    print(f"- Excluded by range filter ({lo_ms}-{hi_ms} ms): {bad.sum()}")
    out[bad] = np.nan
    return out


def generate_3d_plot(ibi_array, label, units, col_name, show=True):
    """Generate and save the 3D delay-coordinate trajectory."""
    # Embedding dimension = 3, delay = 1
    x = ibi_array[2:]    # IBI[t]
    y = ibi_array[1:-1]  # IBI[t-1]
    z = ibi_array[:-2]   # IBI[t-2]

    # NaNs (gaps/outliers) make matplotlib break the line instead of bridging them
    n_valid = int((~np.isnan(x + y + z)).sum())
    if n_valid < 1:
        raise ValueError(f"No valid 3-beat sequences available for {label}.")
    print(f"- Valid 3D points: {n_valid}")

    fig = plt.figure(figsize=(10, 8))
    ax = fig.add_subplot(111, projection="3d")
    ax.plot(x, y, z, linewidth=0.8, color="b")

    ax.set_xlabel(f"IBI[t] ({units})")
    ax.set_ylabel(f"IBI[t-1] ({units})")
    ax.set_zlabel(f"IBI[t-2] ({units})")
    ax.set_title(f"3D IBI Delay-Coordinate Plot\nRecord: {label} | Feature: {col_name}")
    plt.tight_layout()

    OUTPUT_DIR.mkdir(exist_ok=True)
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    output_path = OUTPUT_DIR / f"{label}_3D_{timestamp}.png"
    plt.savefig(output_path, dpi=300)
    print(f"Plot saved successfully to: {output_path}")
    if show:
        plt.show()
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description="3D IBI delay-coordinate plot (IBI = 2nd CSV column).")
    ap.add_argument("csv", nargs="?", help="CSV file (omit to search and choose interactively)")
    ap.add_argument("--column", type=int, default=IBI_COLUMN, help="1-based IBI column (default 2)")
    ap.add_argument("--min-ibi", type=float, default=DEFAULT_RANGE_MS[0], help="min IBI in ms (default 300)")
    ap.add_argument("--max-ibi", type=float, default=DEFAULT_RANGE_MS[1], help="max IBI in ms (default 2000)")
    ap.add_argument("--no-filter", action="store_true", help="plot every IBI, no range filter")
    ap.add_argument("--no-show", action="store_true", help="save PNG only, do not open a window")
    args = ap.parse_args()

    print(f"Project Root: {PROJECT_ROOT}")
    selected = Path(args.csv).resolve() if args.csv else select_file(get_candidate_files(PROJECT_ROOT))
    print(f"\nLoading dataset: {selected}")
    df = pd.read_csv(selected)

    lo, hi = (None, None) if args.no_filter else (args.min_ibi, args.max_ibi)

    # Combined file (first column 'record'): one separate plot per record
    if str(df.columns[0]).lower() == "record":
        groups = [(f"{selected.stem}_{rec}", g.reset_index(drop=True)) for rec, g in df.groupby("record", sort=True)]
    else:
        groups = [(selected.stem, df)]

    for label, g in groups:
        print(f"\n=== {label} ===")
        ibi, units, col_name = get_ibi_series(g, args.column)
        ibi = apply_range(ibi, units, lo, hi)
        generate_3d_plot(ibi, label, units, col_name, show=not args.no_show)


if __name__ == "__main__":
    main()
