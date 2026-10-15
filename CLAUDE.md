# Working on Pedaleira

## Plans and decisions

The roadmap, the decisions log, the UI plan, the grid design and the repo description are
kept in a private document, not in this repository:
https://claude.ai/code/artifact/336f856e-bbb8-4daa-a864-6eacf90fbcde

Read it at the start of a session and keep it current as work lands. Never add plan or
decision files to the repository (they are in .gitignore).

## Rules

- No AI attribution anywhere: commit messages, authors, pull requests, code or docs.
- No em or en dashes in code, comments, docs or UI text.
- Commits: subject under 70 characters, a body of 3 to 4 lines. Add files by name, never
  `git add -A`.
- No Neural DSP code or models, no decompiling or decrypting. Own identity: nothing copied
  from another product's look.
- Never name a variable `small`, `near`, `far`, `min`, `max` or `interface` (Windows macros).
- Never regenerate anything under `test/files`.
- The UI talks to the engine only through the command layer (`docs/commands.md`); the
  design canvas is the spec for every screen.
- Agents and models: cheaper models are fine for well-specified, low-risk work (plumbing,
  import tables, tests, docs). Use the strongest model for work judged by expertise or ear
  (amp, cab and other sound-defining DSP, fitting, design). Never trade quality for cost:
  review every agent's output, verify it with tests or measurements, and redo it with the
  stronger model if it is not clean.

## Build and test (Linux)

```sh
cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=cmake/linux-toolchain.cmake -DBUILD_AAX=OFF -DBUILD_CLAP=OFF
cmake --build build
CI=1 ctest --test-dir build/test --timeout 300 -j4
```

Add `-DPEDALEIRA_BUILD_UI_TESTBED=ON` for the UI testbed (run it under `xvfb-run -a`).
`docs/commands.md` is generated: `PedaleiraRender --describe-markdown > docs/commands.md`.
