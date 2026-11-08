---
name: prompt
description: Turn a rough, casual request into a precise, well-grounded prompt (goal, real repo context, constraints, deliverables, definition of done), show it, then run it on approval. Use when the user types /prompt, or asks to "make this a prompt", "write me a prompt for", "turn this into a better prompt", or "prompt-ify" something.
argument-hint: "<what you want, in plain words>  [--run | --save | --raw]"
---

# /prompt — rough request in, sharp prompt out

The user gives you a loose request ("the save menu is slow, fix it", "add co-op
chat"). Your job is to turn it into the prompt a careful senior engineer would
want to receive, grounded in **this** repository, then (on approval) carry it out.

What makes a prompt good is not "Act as a senior X". The role line helps a
little; what helps a lot is:

1. **A concrete goal**: the outcome, stated so you could tell if it was achieved.
2. **Real context**: actual file paths, build/test commands, conventions, prior
   decisions, taken from the repo and never invented.
3. **Constraints and non-goals**: what must not change, what is out of scope.
4. **Deliverables**: exactly what comes back (code, a report, a plan, a diff).
5. **Definition of done**: how the result gets verified (a command, a test, a
   manual check).
6. **Working style**: whether to ask first or just act, how big a change is
   acceptable, and what to do when blocked.

## Flags

Parse these out of `$ARGUMENTS` before anything else:

- `--run`: skip the approval step and carry out the prompt right away once it is written.
- `--save`: also write the prompt to `.claude/prompts/<short-slug>.md` so it can be reused.
- `--raw`: output only the prompt in a code block, with no notes and no follow-up question
  (handy for pasting into another chat).

If `$ARGUMENTS` is empty, ask the user what they want in one short question and stop.

## Steps

### 1. Classify the request

Choose the closest **mode** from `templates.md` in this skill's folder: build,
debug, audit, refactor, performance, security, architecture, frontend/UI,
devops, tech-lead/plan, reverse-engineering, explain/research, or multi-role
team. Read only that section of `templates.md`. If the request spans two modes
(for example "find out why X crashes and fix it" covers debug and build), use
the primary one and borrow checklist items from the other.

### 2. Ground it in the repo (fast; about 5 tool calls at most)

Do just enough looking to replace guesses with facts:

- `CLAUDE.md`, `README.md`, and any plan/status file whose name matches the topic.
- The files, functions, or directories the request is plainly about (use Grep/Glob).
- How to build and test: CMakePresets, build scripts, package.json, a Makefile, CI config.

Record real paths, commands, and names. If something can't be found, don't make
it up: list it as an assumption or an open question.

### 3. Close the gaps that matter

List what is still unknown. For each unknown, ask whether a different answer
would change the work significantly.

- **Yes, and you can't infer it**: ask, using AskUserQuestion, at most 3
  questions in a single call, each with concrete options and your recommended
  option first.
- **No, or a sensible default exists**: pick the default and record it under
  *Assumptions*.

Ask nothing when `--run` or `--raw` is set; use defaults and list the assumptions.

### 4. Write the prompt

Use this skeleton. Leave out any section that would be empty, and keep the whole
prompt tight: every line should change what gets done.

```
<One-sentence role tied to the job, e.g. "You are working as the engine
maintainer debugging a crash in the PS2 port.">

## Goal
<The outcome in 1–3 sentences. Make it testable.>

## Context
- Repo: <name>, <language/stack>, built with `<real command>`
- Relevant code: `<path>` (<what it does>), `<path>` ...
- Background: <prior decisions, linked plan docs, known quirks>

## Task
<Numbered steps, ordered the way a senior engineer would work: investigate,
decide, change, verify. Take these from the mode's checklist, pick only the
items that apply, and phrase them for THIS request.>

## Constraints
- <What must not break or change; behavior to preserve>
- <Scope limits / non-goals>
- <Style: match existing conventions in `<path>`>

## Deliverables
- <Exact outputs: code changes, a report with sections X/Y, a plan, etc.>

## Done when
- <Verification: command passes, test added and green, repro no longer reproduces>

## Working style
- Verify assumptions against the code before relying on them; don't guess.
- <Ask-first vs. act-first; whether to commit/push; how to report blockers>
```

Quality bar before showing it:
- Every path and command in it exists (you saw it in step 2) or is marked as an assumption.
- The goal is specific enough that two engineers would build the same thing.
- There is at least one concrete verification step.
- There is no padding: no "make it production-ready and scalable to millions of users"
  unless the user actually needs that. Fit the scale to the project.

### 5. Present it

Unless `--raw` is set, reply with:

1. The prompt in a single fenced code block (so it's easy to copy).
2. **Assumptions** I made: short bullets (omit the section if there are none).
3. One line: *Run it now, tweak it, or save it?*

If `--save` is set, write the file and give its path.
If `--run` is set, or the user approves, treat the prompt as your instructions
and carry it out fully in this session. Follow the user's standing rules from
CLAUDE.md and the session (branch, commit, and push conventions) over anything
the prompt says.
