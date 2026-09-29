# Contributing back: upstream issues for the bugs OrcaMCP fixed

A brief for a fresh, short session. Keep it cheap: no builds, no app launches, no printer. Read only
what this brief points to.

## The decision (the user, 2026-09-29)

We contribute back through **issues on OrcaSlicer/OrcaSlicer, not pull requests**. Every issue has
the same three parts, in this order:

1. **Contributing back.** Two or three sentences: OrcaMCP (https://github.com/okets/OrcaMCP) is a fork
   of OrcaSlicer that adds an MCP server so AI agents can drive the slicer; we found and fixed this bug
   in upstream code along the way, and offer the fix here.
2. **The bug.** Symptom as a user meets it, how to reproduce it, and the root cause (file and
   function in upstream's tree).
3. **Our fix.** A link to the commit in our repo (`https://github.com/okets/OrcaMCP/commit/<sha>`),
   and one sentence on what it changes. Free to take as is or adapt.

Plain, factual, short. American spelling.

## Candidates

Each has a paragraph in CLAUDE.md, "Carried upstream fixes", under its probe letter: symptom, root
cause and our fix are already written there. Adapt that paragraph; write nothing from scratch.

| Probe | Bug | Harm |
|---|---|---|
| AH | The first slice of a session can hang the app (TBB barrier in `name_tbb_thread_pool_threads_set_locale`) | hang |
| O | Quit, New Project or Open during a slice crashes: `Plater::priv::reset` frees the prints before it stops the slice | crash |
| P | Deleting a plate (or undo) during a slice frees the `Print` the slicing thread uses | crash |
| BD | `Print::apply` decides the prime tower from the previous apply's filaments, so the next update throws a finished slice away | lost slice |
| BG | The Preview's layer slider erases filament changes on a plate printing with several filaments, and writes that into the project | lost edits |
| BL | admesh reads a small binary STL whose bytes are all below 128 as ASCII: the model loads empty | wrong load |
| BQ | `AppConfig::set(section, key, "…")` takes the bool overload, so literal values are saved as "true" | lost settings |
| AN | A plate's spiral vase gives its settings to the first object only | wrong print |

The user picks about five; offer this list and a one-line recommendation each.

## Steps

1. Check each candidate is still open upstream: run CLAUDE.md's probe block lines for the chosen
   letters (`git fetch upstream` first). A "fixed" reading means drop it.
2. Search for duplicates: `gh search issues --repo OrcaSlicer/OrcaSlicer "<key words>"`. If one exists,
   comment there instead (same three parts).
3. Find our fix commit: `git log --oneline mcp -S '<a function name from the paragraph>'`, or the commit
   subjects around the probe's release item.
4. Draft all texts into one file and show it to the user. **File nothing before the user approves the
   texts.**
5. File with `gh issue create -R OrcaSlicer/OrcaSlicer` (always pass `-R`: without it `gh` on this fork
   may pick the wrong repository). Record each issue's URL next to its probe line in CLAUDE.md, in a
   `[skip ci]` commit.
