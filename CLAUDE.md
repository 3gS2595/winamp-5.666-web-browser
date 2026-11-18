## Communication

- If my code, idea or assumption is wrong, say so and why before building on it. No softening or reassurance.

## Code

- Match the codebase: its layout, naming, helpers and libraries. Don't introduce a new pattern or dependency for something it already handles another way.
- Before writing new logic, search for existing code that does it and reuse or extend that.
- When logic is needed in a second place, move it to wherever the codebase keeps shared code of that kind and switch every caller to it.
- Generalize by turning what varies into parameters, not boolean flags or mode switches that branch inside the function. If two uses share only a core, extract the core and leave the differences at the call sites.
- Don't abstract for a single caller. A helper, option or extension point waits for a second real use.

## Design problems

- A design problem is one that gets worse as code is added: duplicated logic, logic owned by the wrong module, a leaky abstraction, a pattern that has to be worked around. if you ever notice this stop and discuss how to fix it with me before it gets worse.
- If the code you're about to build on, extend or copy has one, or there's no clean place for what you're adding, stop before writing. Say what's wrong, propose the fix and wait for my call. Don't build on it and don't invent a one-off location.

## Replacing code

- treat the project as an unreleased first version: nothing has to keep working for anyone else. When something is replaced, delete the old version and update every call site. No shims, adapters, fallbacks or legacy branches, and delete anything inside the project folder your change makes dead.

## Git

- Commit your own work without being asked when a feature, fix or refactor is complete and the project's checks pass. One logical unit per commit, so commit the finished unit before starting something unrelated.
- Also commit before a risky or sweeping edit, even mid-task, as long as the checks pass at that point. That commit is the rollback point.
- Commit messages: imperative mood, say why the change was made.
- Don't push, amend, rebase or force unless I ask you to.
- No AI attribution anywhere: no Co-Authored-By trailer, no "Generated with Claude Code" line.
