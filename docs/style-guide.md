<!-- Keep formatting commands synchronized with tools/format.py and CI. -->

# C++ style guide

Use the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html)
for new and modified C++ code. The repository's `.clang-format` applies the Google
preset, including two-space indentation, 80-column wrapping, brace placement,
pointer spacing, and include ordering. Formatting does not rename existing public
APIs; preserve compatibility when applying naming recommendations.

Install clang-format 18 and format every maintained C/C++ header, implementation,
and Protocol Buffers file:

```sh
sudo apt-get install clang-format-18
python3 tools/format.py
python3 tools/format.py --check
```

Set `CLANG_FORMAT` if the version-18 executable has a different path. CI checks
formatting with the same major version. Generated files, external dependencies,
and recorded measurements are excluded. clang-format does not support CMake,
Python, shell, YAML, Markdown, launch descriptions, linker scripts, or GNU assembly;
retain those formats' existing conventions.

Use comments to explain intent, ownership, synchronization, and constraints.
Keep comments accurate when behavior changes, and avoid narrating obvious code.
