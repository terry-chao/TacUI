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
resolve. `0.1.0` is done — tag pushed, hash filled in, and all three triplets
built from that tarball. This is the recipe for the next version:

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
triplet the `supports` field allows, which is the x64 Windows set above and
nothing else. `x64` is in the expression on purpose — the renderer is D3D12 and
the text stack is DirectWrite, and the library has only ever been built for
x64. Add `arm64` back once a machine has actually produced a binary that runs
there.

## Distributing without the official registry

An overlay port needs a local directory, which means every consumer has to
clone this repository. A git registry removes that: vcpkg clones the registry
itself, so consumers only need a repository URL and a baseline commit.

The layout is already here — `ports/tacui/` plus `versions/`, which is the
version database (`versions/t-/tacui.json` and `versions/baseline.json`).
Consumers write this into their own `vcpkg-configuration.json`:

```json
{
  "registries": [
    {
      "kind": "git",
      "repository": "https://github.com/terry-chao/TacUI",
      "reference": "main",
      "baseline": "<commit sha>",
      "packages": ["tacui"]
    }
  ]
}
```

The `baseline` is a commit SHA, and vcpkg reads `versions/baseline.json` at
that commit — so it has to be a commit that already contains `versions/`.

### Maintaining it

`versions/t-/tacui.json` records, per version, the `git-tree` of the
`ports/tacui` directory. Recompute it with:

```sh
git rev-parse HEAD:ports/tacui
```

Every time `ports/tacui/` changes, that hash changes too. Either bump the
version in `ports/tacui/vcpkg.json` and add a new entry, or — for a change that
does not alter what gets built, like a comment fix — bump `port-version` and
update the existing entry's `git-tree`. Forget this and vcpkg builds a port
that does not match the one in the tree, which is a confusing way to spend an
afternoon.
