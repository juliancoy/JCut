# Repository Instructions

## Development workflow

- Do not create additional Git branches.

## Build policy

- Use `./build.sh` from the repository root for all builds and build verification.
- Do not invoke CMake, Ninja, Make, or other underlying build tools directly unless the user explicitly requests it or `./build.sh` cannot perform the required task.
- When reporting build results, include the exact `./build.sh` command and any options used.
