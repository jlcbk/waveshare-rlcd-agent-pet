#!/usr/bin/env python3
"""Derive the battery-percent voltage lookup table in src/battery_monitor.cpp.

Source data (download manually, ~3 MB):
  University of Bayreuth, "Cell-to-Cell Variations of a Panasonic NCR18650B",
  Zenodo record 8369275 — 92 new cells, 25 C, 0.05C discharge.
  https://zenodo.org/records/8369275
    pocv_soc.csv — shared SOC axis for the rows of pocv_dc.csv (0..1)
    pocv_dc.csv — per-cell discharge pseudo-OCV, one column per cell

Method (see the comment block on kVoltagePercentCurve):
  1. median voltage across the 92 cells at each SOC row;
  2. read the median curve's SOC at 3.60 V and at 4.10 V;
  3. normalize SOC between those two points into 0..100 % and sample every
     50 mV. The 3.60–4.10 V span is a user-chosen usable-voltage display
     window, not the cell's physical 0–100 % SOC range.

Usage: python3 derive_points.py /path/to/pocv_soc.csv /path/to/pocv_dc.csv
"""

import csv
import statistics
import sys

WINDOW_LO_V = 3.60
WINDOW_HI_V = 4.10
STEP_MV = 50


def read_columns(path):
    with open(path) as handle:
        for row in csv.reader(handle):
            if not row:
                continue
            yield [float(value) for value in row]


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    soc = next(read_columns(sys.argv[1]))
    medians = [statistics.median(row) for row in read_columns(sys.argv[2])]
    if len(soc) != len(medians):
        sys.exit("soc axis and dc rows disagree")

    def soc_at(volts: float) -> float:
        for i in range(1, len(medians)):
            if medians[i - 1] <= volts <= medians[i]:
                span = medians[i] - medians[i - 1]
                frac = (volts - medians[i - 1]) / span
                return soc[i - 1] + frac * (soc[i] - soc[i - 1])
        raise ValueError(f"{volts} V outside the measured curve")

    soc_lo = soc_at(WINDOW_LO_V)
    soc_hi = soc_at(WINDOW_HI_V)
    print(f"# soc@{WINDOW_LO_V:.2f}V = {soc_lo * 100:.2f}%  "
          f"soc@{WINDOW_HI_V:.2f}V = {soc_hi * 100:.2f}%  "
          f"window = {(soc_hi - soc_lo) * 100:.2f}% SOC")
    print("mv   pct")
    for mv in range(int(WINDOW_LO_V * 1000), int(WINDOW_HI_V * 1000) + 1, STEP_MV):
        normalized = (soc_at(mv / 1000) - soc_lo) / (soc_hi - soc_lo)
        print(f"{mv}  {round(normalized * 100)}  # {normalized * 100:.2f}")


if __name__ == "__main__":
    main()
