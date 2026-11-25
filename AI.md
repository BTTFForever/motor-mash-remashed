# How this project was made

**Full disclosure: the code and documentation in this repository were written by AI, under
human direction.** The project's author led the development with Anthropic's Claude, working
in Claude Code:
- set the goals and priorities, and decided what to build and how it should look and feel;
- playtested every change on real hardware (keyboard, controllers, both renderers);
- reported the bugs and the details that were off, and judged the results;
- approved each commit and every release.

Claude did the reverse engineering of the original game, wrote the patch DLL, the Vulkan
renderer, the tools and the documents, and tested its own work where it could, by driving
the game and reading its memory. Nothing was merged or released without the author's
sign-off.

## Which models contributed

| Model | Share | Role |
|---|---|---|
| Claude Opus 5.5 | about 96.5% | the main development sessions, plus some subagents |
| Claude Haiku 5.5 | about 3.5% | subagents for searching and reading |

**How this was measured.** The shares are the models' output tokens in the Claude Code
session logs for this project, from 21 to 25 November 2026. Each reply is counted once,
and the logs cover every commit in this repository.

**What the shares leave out:**
- the author's own work, none of which shows up as tokens: direction, testing, decisions,
  and the author's original porting notes, which [PORTING.md](PORTING.md) grew from;
- the earlier track viewer used for the custom track research, which is not part of this
  repository.

Read the shares as which model wrote what, not as how much of the project was human
versus AI.

The commits say the same thing in their `Co-Authored-By: Claude` trailers.
