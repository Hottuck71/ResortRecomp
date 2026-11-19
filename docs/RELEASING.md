# Publishing on GitHub

## The first time

1. On github.com, signed in: **New repository**, name it (for example `ResortRecomp`), public, **without** a
   README, licence, or .gitignore (this folder has them).
2. In this folder (PowerShell), with the address GitHub shows:

```
git add .
git commit -m "ResortRecomp 1.0.0"
git branch -M main
git remote add origin https://github.com/<your-username>/ResortRecomp.git
git push -u origin main
```

## Each release

1. Build the toolkit: `powershell -ExecutionPolicy Bypass -File scripts\build-toolkit.ps1 -Version 1.0.0`
2. On GitHub: **Releases > Draft a new release**, tag `v1.0.0`, and attach `exports\ResortRecomp-1.0.0.zip`
   (GitHub allows files up to 2 GB).

## Never upload

- The game, a disc image, or anything extracted from it.
- A built `ResortRecomp-Game.exe` (or any other build of the game), or the `build\` folder: they're made from the
  game's code.
- Save files, shader caches, or reports from playing.

The repository's `.gitignore` keeps these out, and `scripts\66_make_toolkit.ps1` refuses to make a toolkit that
contains any of them. Before each push, `git status` shows exactly what would go up.
