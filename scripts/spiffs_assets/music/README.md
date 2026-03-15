Put local song files here for default firmware builds.

Required filenames:
- `song1.ogg`
- `song2.ogg`
- `song3.ogg`

How it works:
- `main/CMakeLists.txt` passes this folder as `--extra_files` to `scripts/build_default_assets.py`.
- During build, these files are packed into `assets.bin` and flashed to the `assets` partition.

Voice commands:
- `nyanyi` / `putar lagu` -> plays `song1.ogg`
- include `dua`/`2` -> `song2.ogg`
- include `tiga`/`3` -> `song3.ogg`
