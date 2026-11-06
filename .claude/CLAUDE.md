# CLAUDE.md

Standing rules for AI assistants working in this repository.

## Commits and pull requests
- Never credit Claude or any AI assistant: no `Co-Authored-By` or session trailers, no session links, no "Generated with" lines, and no AI author or committer, in commits, PRs or files. Commits show only the maintainer.
- Author and committer email: `59450931+jbuck2005@users.noreply.github.com`. Set it before the first commit, or the commit is authored by the assistant.
- Name branches `dev/<topic>`. Never put "claude" in a branch name: merging copies it into the history of `main`.
- Keep "claude" and "anthropic" out of commit messages and PR titles, including for changes to this file (write "project rules").
- Stage files by name. Never `git add -A` or `git add .`.
- Never force-push `main`.
- One PR per change, and the owner merges it. Never merge your own PR: this controls real water valves.

## Privacy and safety
- The repo is public. Nothing home-specific belongs in it: no IPs, hostnames, paths, names or credentials.
- Never put a real token in code, docs or examples. Use `your-long-random-token`. There is no default token: every install sets its own.
- GitHub Actions secrets are referenced by name only. Never paste a secret's value into a workflow file.
- Assistants can't test on real hardware. Say so in PRs that change valve or relay behaviour.

## Checks
- Run `make` before committing.
