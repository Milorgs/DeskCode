This workspace is a git repository whose current branch is `main`.

1. Create a new branch named `feature/polite-greeting` from the current `main` commit.
2. On that branch, change `greet()` in `greet.py` so that `greet("Ada")` returns
   `"Hello, Ada!"` (it currently returns `"Hello Ada"`), and add the bullet line
   `- Polite greeting punctuation` directly under the `## Unreleased` heading in
   `CHANGELOG.md`.
3. Commit both changes as exactly **one** commit on `feature/polite-greeting` with the commit
   message subject `feat: polite greeting`.
4. Switch back to `main` and leave it unchanged (no new commits on `main`, no merge). The
   working tree must be clean when you finish.

If git asks for an identity, configure one locally in this repository only
(`git config user.name ...` / `git config user.email ...`), never globally.
