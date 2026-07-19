# Outlaws Reimagined

**Outlaws Reimagined** is an open-source reimplementation of the classic LucasArts game **Outlaws**.

I grew up playing this game, so this project is my attempt to bring it back on modern platforms with a clean, fully open-source implementation.

## Project Status

The project is still a work in progress.

Most of the engine is being reverse-engineered using **Ghidra**, with the goal of reproducing the original game's behavior as accurately as possible. My current focus is the scripting system, which is responsible for mission logic and objectives. Several campaign objectives are already working, but there is still a long way to go.

One of the main goals of this project is to stay as faithful as possible to the original engine. I'm intentionally avoiding game-specific hacks or shortcuts whenever possible, as they tend to make the codebase harder to understand and maintain in the long run.

## How it works

This project is primarily based on reverse engineering of the original executable using **Ghidra**. Whenever possible, the original engine's behavior is reproduced instead of approximated, making the project both accurate and easy to compare against the original implementation.

The long-term goal is to provide a modern, portable, open-source implementation of the Outlaws engine.

## Acknowledgements

A huge thank you to the people whose work made this project possible:

- **glampert** for documenting and reverse-engineering the **LAB (LucasArts Archive Binary)** format. This project builds heavily on that research:
  https://github.com/glampert/reverse-engineering-outlaws

- **The Force Engine** team for their excellent open-source implementation of the Jedi Engine:
  https://theforceengine.github.io/

  While Outlaws uses a more advanced version of the Jedi Engine than the one used in *Dark Forces*, The Force Engine has been an invaluable reference for understanding many engine internals, including systems such as portal culling and other rendering behaviors.
