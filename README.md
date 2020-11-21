# Okage: Shadow King — native macOS port

Native Apple Silicon macOS port of the 2001 PS2 game (SCUS-971.29),
built by decompilation and reimplementation. Requires an owned retail
disc for assets.

## Build

```sh
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Release
cmake --build build/native -j8
```

## Test

```sh
ctest --test-dir build/native
python3 -m unittest discover -s tests -v
```

Not affiliated with Sony or Zener Works.
