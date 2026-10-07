# ports/

An overlay port for TacUI itself. It exists so the library can be consumed the
way everything else on Windows is consumed:

```sh
vcpkg install tacui --overlay-ports=C:/dev/TacUI/ports
```

Nothing here is built by TacUI's own CMake — `vcpkg install` reads
`tacui/vcpkg.json`, downloads the source at the tag matching `version-semver`,
and builds it with the install rules in the top-level `CMakeLists.txt`.

## Publishing it

The port points at a **git tag**, so the tag has to exist before the port can
resolve. The order is:

1. Land the install rules and the version bump on `main`.
2. `git tag v0.1.0 && git push origin v0.1.0`.
3. Run the install once; vcpkg reports the tarball's real hash:

   ```sh
   vcpkg install tacui --overlay-ports=C:/dev/TacUI/ports
   ```

   Copy the `SHA512` it prints into `tacui/portfile.cmake`.
4. Verify all three Windows triplets:

   ```sh
   vcpkg install tacui:x64-windows             --overlay-ports=C:/dev/TacUI/ports
   vcpkg install tacui:x64-windows-static      --overlay-ports=C:/dev/TacUI/ports
   vcpkg install tacui:x64-windows-static-md   --overlay-ports=C:/dev/TacUI/ports
   ```

   The static triplets are the ones that bite: they set `BUILD_SHARED_LIBS=OFF`,
   which turns `tacui` from a DLL into a static library and the C ABI's
   `TUI_BUILD_DLL` off with it.

## Submitting to microsoft/vcpkg

Only worth doing once the API is frozen — every change to the port needs
`vcpkg x-add-version tacui` re-run, and the review is slow. When it is time:

```sh
git clone https://github.com/microsoft/vcpkg
cp -r ports/tacui vcpkg/ports/tacui
cd vcpkg && ./bootstrap-vcpkg.sh
./vcpkg x-add-version tacui      # writes versions/t-/tacui.json and the baseline
```

Then open a PR containing `ports/tacui/` and `versions/`. CI builds every
triplet the `supports` field allows — `windows & !uwp` means it will also try
`arm64-windows`, which has never been built here. Narrow `supports` if that
first CI run fails.

## Distributing without the official registry

An overlay port works for one machine. For a team, register it as a git
registry instead — same port directory, but vcpkg tracks it in
`vcpkg-configuration.json`:

```json
{
  "registries": [
    {
      "kind": "git",
      "repository": "https://github.com/terry-chao/TacUI",
      "reference": "main",
      "packages": ["tacui"],
      "baseline": "<commit sha>"
    }
  ]
}
```

A git registry needs a `versions/tacui.json` alongside `ports/` and the port
directory at the repository root, which this repo does not have yet.
