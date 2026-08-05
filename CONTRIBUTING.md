# Contributing to Meccha Camouflage

Thanks for contributing. Keep each change focused, explain the user-visible
effect, and include the verification that supports it.

## Before You Start

- Use an issue to discuss a non-trivial feature, behavior change, or bug before
  implementing it.
- Report suspected vulnerabilities through the private process in
  [SECURITY.md](SECURITY.md), not in a public issue or pull request.
- Do not include generated output, packaged executables, injected DLLs, game
  captures, logs, or credentials in a commit.

## Development Setup

The app and its direct bridge target Windows 10 and Windows 11. A local build
needs:

- Git and PowerShell
- .NET SDK 10
- Visual Studio 2022 Build Tools with the x64 C++ toolchain
- GNU Make, or direct use of the PowerShell build script

Clone the repository, then build it:

```bash
git clone --recurse-submodules https://github.com/acentrist/MecchaCamouflage.git
cd MecchaCamouflage
make build
```

For an existing checkout, initialize the pinned dependencies before building:

```bash
git submodule update --init --recursive
```

Without GNU Make, run the equivalent from PowerShell:

```powershell
.\scripts\build.ps1
```

The build runs the C# test suite and writes generated files only under
`.build/`. For a faster test-only iteration, run:

```bash
dotnet run --project src/csharp/MecchaCamouflage.Tests/MecchaCamouflage.Tests.csproj --no-launch-profile
```

## Change Guidelines

- Keep runtime, bridge, GUI, research, and packaging changes separate unless
  they form one deliberate migration.
- Preserve reflection names, ABI layouts, padding, command strings, and other
  dynamic runtime contracts unless the change has focused verification. See
  [Runtime maintenance](docs/runtime-maintenance.md).
- Add or update focused tests for changed behavior. Web UI additions must keep
  all declared localization keys available in every supported locale.
- Run `make build` before opening a pull request. Changes affecting injection,
  paint behavior, or multiplayer replication also need an appropriate Windows
  game smoke test.
- Update documentation when behavior, prerequisites, configuration, or the
  verification process changes.

## Pull Requests

Open a pull request with:

1. A concise description of the problem and solution.
2. Linked issues, when applicable.
3. The checks you ran and their results.
4. Screenshots for visible UI changes and reproduction steps for bug fixes.
5. No unrelated formatting, generated files, or binary artifacts.

Contributions are provided under the repository's
[GPL-3.0-or-later license](LICENSE.txt).
