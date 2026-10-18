# aisp-launcher (Ai Sp@ce Launcher)

Parent repo: [aisp-emu](https://github.com/Tricon2-Elf/aisp-emu)

This repo was split off easier pipelines/releases and maintainability

## aisp-emu (Parent project)

aisp-emu is a fan-made server emulator for the discontinued Japanese MMO **Ai Sp@ce**. The goal of this project is to recreate the original experience for educational purposes and to keep the game playable after its shutdown.

Test server running at https://aisp.moe/
Instructions on connecting are on the site
## Project scope

- Provides a replacement launcher for Ai Sp@ce with enhancements (Vulkan, Modern DirectX, Billboard/Screen fixes)
- Serves as a learning resource reverse engineering of legacy online games.
- Does **not** ship original game assets.

## Repository layout

- `aisp.electron/` — Electron configuration scripts for Billboard/screen fixes
- `aisp.hook/` — Injection/Hook for Ai Sp@ce exe for enhancements
- `aisp.launch/` — Launcher itself built in C#
- `scripts/` — Scripts and etc

## Prerequisites

- [.NET 10 SDK](https://dotnet.microsoft.com/en-us/download) for building and running the projects.
- Docker for building the dll hook using mingw32. Could be done on host but just to simplify things was done using a docker container

## Building

From the repository root:

To build the hook dll
```bash
./scripts/build-hook.sh
```

To build the Launcher
```bash
dotnet restore
dotnet build
```

## Contributing

Community contributions are welcome. Please ensure changes respect the educational, non-commercial nature of the project. If you contribute reverse-engineered insights or packet captures, avoid including any proprietary assets.

## Disclaimer

This emulator is an unofficial, community effort provided for educational and preservation purposes. It is not affiliated with the original Ai Sp@ce developers or publishers.

## AI-Assisted Development

This project makes use of AI-assisted development tools to help accelerate certain tasks. This choice is driven by limited development time and other constraints that make traditional pacing difficult. AI assistance allows the project to maintain momentum despite these challenges.
