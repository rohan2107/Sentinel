#!/usr/bin/env python3
"""PreToolUse hook: keep Claude Code off the default branch.

Blocks, for Bash commands only:
  * `git commit` / `cherry-pick` / `revert` while HEAD is master or main
  * any `git push` that would update master or main (explicit refspec, a bare
    push while on one of them, or --all/--mirror)

Why a hook and not just an instruction: CLAUDE.md and memory are context the
model may not follow; a hook is code and always runs. This rule was already
broken once, by a commit made on master out of habit.

Design: quoted strings and heredoc bodies are stripped first, so a commit
message that merely *mentions* `git push origin master` cannot trigger a block,
and a message containing `;` or `&&` cannot hide a real command from the scan.
The scan then finds every `git <subcommand>` invocation in order, tracking
whether an earlier `checkout -b` / `checkout master` in the same line changes
which branch a later commit lands on. When unsure it blocks: a false block costs
one retry, a missed one costs a commit on master.

Exit 2 blocks the call and feeds stderr back to Claude; exit 0 defers to the
normal permission flow. Standard library only.
"""
import json
import re
import subprocess
import sys

PROTECTED = {"master", "main"}
COMMIT_LIKE = {"commit", "cherry-pick", "revert"}

HEREDOC = re.compile(r"<<-?\s*(['\"]?)(\w+)\1[^\n]*\n.*?\n\s*\2\b", re.DOTALL)
DOUBLE_QUOTED = re.compile(r'"(?:\\.|[^"\\])*"')
SINGLE_QUOTED = re.compile(r"'[^']*'")

# `git`, then any global options, then the subcommand and the rest of that
# simple command (up to a separator).
GIT_CALL = re.compile(
    r"\bgit"
    r"(?:\s+(?:-C\s+\S+|-c\s+\S+|--git-dir[=\s]\S+|--work-tree[=\s]\S+|--?[A-Za-z][\w-]*))*"
    r"\s+([A-Za-z][\w-]*)([^\n;&|]*)"
)


def sanitize(command: str) -> str:
    command = HEREDOC.sub("", command)
    command = DOUBLE_QUOTED.sub('""', command)
    return SINGLE_QUOTED.sub("''", command)


def current_branch(cwd: str) -> str:
    """Branch name, or "" when unknown (detached HEAD, not a repo)."""
    try:
        out = subprocess.run(
            ["git", "-C", cwd, "rev-parse", "--abbrev-ref", "HEAD"],
            capture_output=True, text=True, timeout=5,
        )
    except (OSError, subprocess.SubprocessError):
        return ""
    name = out.stdout.strip()
    return "" if out.returncode != 0 or name == "HEAD" else name


def strip_heads(ref: str) -> str:
    return ref[len("refs/heads/"):] if ref.startswith("refs/heads/") else ref


def push_targets_protected(args, branch: str) -> bool:
    if any(a in {"--all", "--mirror"} for a in args):
        return True
    positional = [a for a in args if not a.startswith("-")]
    if len(positional) >= 2:  # <remote> <refspec>...
        return any(
            strip_heads(spec.split(":")[-1].lstrip("+")) in PROTECTED
            for spec in positional[1:]
        )
    return branch in PROTECTED  # bare push sends the current branch


def branch_after_switch(sub: str, args, branch: str) -> str:
    """Where HEAD is after a checkout/switch, as far as we can tell."""
    if any(a in {"-b", "-B", "-c", "-C", "--create", "--force-create"} for a in args):
        return "<new branch>"
    if "--" in args:  # `checkout -- <paths>` restores files, no branch change
        return branch
    positional = [a for a in args if not a.startswith("-")]
    if positional and strip_heads(positional[0]) in PROTECTED:
        return strip_heads(positional[0])
    return branch


def main() -> int:
    try:
        event = json.load(sys.stdin)
    except (ValueError, OSError):
        return 0  # never break the session over unparseable input
    if event.get("tool_name") != "Bash":
        return 0

    command = sanitize((event.get("tool_input") or {}).get("command", ""))
    branch = current_branch(event.get("cwd") or ".")

    for match in GIT_CALL.finditer(command):
        sub, args = match.group(1), match.group(2).split()

        if sub in {"checkout", "switch"}:
            branch = branch_after_switch(sub, args, branch)
        elif sub in COMMIT_LIKE and branch in PROTECTED:
            print(
                f"Blocked: `git {sub}` while on '{branch}'. Create a branch first "
                "(`git checkout -b <type>/<name>`, as its own command), then commit "
                "there and open a PR.",
                file=sys.stderr,
            )
            return 2
        elif sub == "push" and push_targets_protected(args, branch):
            print(
                "Blocked: this push would update master/main directly. Push a "
                "feature branch and open a PR instead.",
                file=sys.stderr,
            )
            return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
