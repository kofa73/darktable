@AGENTS.md

## Issue tracking

Keep track of bugs to fix and experiments to try in `issues/`:

- one file per issue in `issues/`, with its status, goal, context, steps and what "done" means;
  update the file as work progresses
- `issues/README.md` is the index: a table sorted by priority, listing each issue file, a short
  description and its status (`done`, `wip` or `pending`)
- at most one issue is `wip` at a time

Where new bugs, experiments and ideas go:

- **Related to the highlight reconstruction work:** add a file to `issues/` and a row to the
  index. This includes every new experiment or idea, not only bugs.
- **Unrelated darktable bugs:** write them up in
  `/tmp/dt-issues/open/<slug>/<slug>.md`, without a number: Kofa assigns numbers when he submits
  them to GitHub himself. Check `/tmp/dt-issues/` for an existing entry first. Do not fix these:
  the task is to research and experiment with highlight recovery, not to fix existing bugs.
- **Bugs in your own new or modified code:** fix them.

## Commits and branches

- On the `gradient-hl` branch, you may commit without asking.
- You may create sub-branches of `gradient-hl` for experiments and similar work, and commit on
  them too.
- Pushing still needs Kofa's explicit request. AGENTS.md rules still apply: every commit must
  compile, and stage only the files the task touched.

## Temporary files

After completing a task, remove the scratch files it created that no later task needs, whether
that task is an agent's or Kofa's judgement: renders, dumps, copies and build outputs of finished
experiments. Keep what a later task or a pending decision relies on (scores, tables, reports, crop
sheets, renders an open issue will reuse), and note in the issue file what was removed. Files of
other tasks or sessions: ask before deleting.

## Delegating to other agents

Delegate routine and parallelizable work to the agents below. Doing everything yourself uses up
the token budget and halts the work.

Each script in `~/.local/bin` runs one non-interactive task and prints the agent's final message
on stdout, with its progress log on stderr. The script headers list the options.

| script | agent and model | use for |
|---|---|---|
| `ask-luna` | Codex CLI, GPT-6 Luna, effort max | routine implementation and similar work; very cheap, fairly capable; call freely |
| `ask-codex` | Codex CLI, GPT-6.1 Sol (OpenAI's strongest), effort high | harder tasks and reviews |
| `ask-gemini` | agy (Google Antigravity), Gemini 3.8 Flash (High) | affordable, not very reliable; second opinions, quick checks |

- Codex scripts: `ask-luna -C /workspace/darktable -f task.md > answer.md 2> progress.log`.
  Keep the scripts' default model and effort.
- `ask-gemini`: never change the model (`ASK_GEMINI_MODEL`). For any non-trivial task, start the
  prompt with `/boost` on its first line. agy ignores the current directory, so give absolute
  paths in the prompt.
- Review all output: check every claim against the code or the data before acting on it or
  passing it on. Let one agent at a time edit code; tell agents that investigate not to modify
  code, and none of them to commit or push.
- Agent documentation: `~/github-repos-for-agents/codex/`, `~/github-repos-for-agents/antigravity/`.
