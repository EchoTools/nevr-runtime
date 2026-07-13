# modules/

Optional drop-in build-extension point for the top-level build.

Each subdirectory here that contains a `CMakeLists.txt` is auto-discovered by
the root `CMakeLists.txt` and added to the build via `add_subdirectory()`. This
lets you attach extra build targets to the tree without editing the root build
files.

- The tree is **empty by default** — this README is the only tracked file.
- Everything else under `modules/` is ignored by git, so local modules stay
  local and never appear in `git status`.
- Discovery is a no-op when the directory is empty: a normal build is completely
  unaffected if you drop nothing in here.

## Adding a module

Create (or symlink) a subdirectory containing a `CMakeLists.txt`:

```
modules/
  my-module/
    CMakeLists.txt
    ...
```

On the next configure you will see it reported in the CMake STATUS output:

```
-- [modules] adding my-module
```

Each module's `CMakeLists.txt` should define its own targets. Keep it
standalone-configurable where practical so it can also be built in isolation.
