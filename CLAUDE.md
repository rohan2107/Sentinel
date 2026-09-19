@AGENTS.md

## Claude Code specifics

- `.claude/settings.json` registers a `PreToolUse` hook that blocks `git commit`
  (and `cherry-pick`/`revert`) while on `master`/`main`, and any `git push` that
  targets them. If it blocks you, create a branch in a separate command first.
- Before saying a change works, run `./scripts/test.sh --config Both` and quote
  the result. A passing summary is not evidence; the tests running and able to
  fail is.
