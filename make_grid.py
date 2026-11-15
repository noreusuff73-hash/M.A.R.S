"""
Turn a Mars elevation file into mars_elev.bin (1440 x 720, 0.25 degree cells, metres).

Usage:
    python make_grid.py <input file>

Input can be:
  - a GeoTIFF (.tif) such as the USGS HRSC/MOLA blended DEM, or
  - a NetCDF grid (.grd / .nc) such as the GMT mars_relief files.

Install once:
    pip install numpy xarray scipy netcdf4 rasterio

Put the output mars_elev.bin in the same folder as index.html.
"""
import sys
import numpy as np

W, H = 1440, 720
src = sys.argv[1]
lats = 90 - (np.arange(H) + 0.5) * 180 / H       # north -> south
lons = -180 + (np.arange(W) + 0.5) * 360 / W     # west -> east

if src.lower().endswith((".tif", ".tiff")):
    import rasterio
    from rasterio.enums import Resampling
    with rasterio.open(src) as ds:
        a = ds.read(1, out_shape=(H, W), resampling=Resampling.average).astype("float64")
        if ds.nodata is not None:
            a[a == ds.nodata] = np.nan
        left = ds.bounds.left
    if left >= -1:                       # file runs 0..360 east: shift to -180..180
        a = np.roll(a, W // 2, axis=1)
else:
    import xarray as xr
    ds = xr.open_dataset(src)
    z = ds["z"] if "z" in ds else ds[list(ds.data_vars)[0]]
    latn = [n for n in z.dims if n.startswith("lat") or n == "y"][0]
    lonn = [n for n in z.dims if n.startswith("lon") or n == "x"][0]
    lon_t = lons % 360 if float(z[lonn].max()) > 180 else lons
    a = z.interp({latn: lats, lonn: lon_t}).values

a = np.nan_to_num(a, nan=0.0)
out = np.clip(np.round(a), -32768, 32767).astype("<i2")
out.tofile("mars_elev.bin")
print("Wrote mars_elev.bin", out.shape, "min", int(out.min()), "max", int(out.max()), "metres")
print("Sanity check: expect roughly -8200 (Hellas) to +21000 (Olympus Mons).")
