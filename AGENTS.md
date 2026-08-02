# Agent Workflow

## Required issue and pull-request workflow

Every single repository change must follow this lifecycle, in order:

1. File a scoped GitHub issue before modifying repository files.
2. Implement and test the change on a dedicated branch.
3. Commit the implementation with the issue number in the commit message.
4. Open a pull request that links to the issue without closing it.
5. Merge the pull request.
6. Close the issue only after confirming that the pull request was merged.

Keep each issue and pull request scoped to one coherent change. Do not commit
directly to the default branch, close an issue before its pull request is merged,
or leave a merged change's issue open. File a new issue for every bug,
regression, or follow-up discovered later.

## Running the game

- Build and test changes when practical.
- Do not restart or relaunch the game unless Mike explicitly asks.
