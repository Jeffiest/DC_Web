# Mode templates for /prompt

Each mode has a **role** line, a **checklist** of task steps to choose from (use
only the ones that apply and rewrite them for the specific request),
**deliverables**, and a **done when** default. The modes are based on
popular "senior engineer" prompt patterns, tightened up: fewer buzzwords, more
verifiable steps.

---

## build — new feature or MVP
Role: engineer adding <feature> to <project>, owning it end to end.
Checklist:
- Restate the feature as user-visible behavior, including edge cases (empty, error, limits).
- Find the closest existing feature and follow its structure and conventions.
- Sketch the design first: data model or schema changes, APIs/entry points, file layout.
- Build the smallest version that fully works, then stop. List possible extensions; don't build them.
- Add or extend tests next to the existing ones.
Deliverables: a short design note, the code, tests, and how to try it.
Done when: it builds, the tests pass, and the feature can be demonstrated with <command/steps>.

## debug — something is broken
Role: debugging engineer tracking down a production issue.
Checklist:
- Reproduce first: get the exact symptom, inputs, and environment. If it can't be reproduced, say so and say why.
- Trace the actual execution path; read the code, don't guess.
- State the root cause with evidence (a log line, a value, a commit).
- Check the edge cases and other callers affected by the same cause.
- Fix the cause, not the symptom, with the smallest correct change.
- Add a regression test or a repro script.
Deliverables: what fails and why, the root cause, the fix, and the edge cases checked.
Done when: the repro no longer reproduces, the regression test passes, and nothing else changed behavior.

## audit — understand and assess a codebase
Role: senior engineer who just joined and is mapping an unfamiliar codebase.
Checklist:
- Reverse-engineer the architecture and the end-to-end data flow for <area>.
- Find duplicated logic, dead code, tight coupling, bottlenecks, and risky areas.
- Rank the findings by impact and by the effort to fix.
- Don't change code unless asked; the output is the map plus recommendations.
Deliverables: an architecture breakdown (components and how data flows between them), a ranked problem list with file:line references, and refactoring strategies.
Done when: every finding cites a location and a concrete consequence.

## refactor — clean up without changing behavior
Role: architect restructuring <area> for maintainability.
Checklist:
- Pin down the current behavior first (existing tests, or add characterization tests).
- Improve separation of concerns, modularity, and coupling in small, reviewable steps.
- Keep public interfaces stable unless told otherwise.
- Run the tests after each step.
Deliverables: before/after structure, the refactored code, and the key improvements made.
Done when: behavior is identical (tests green before and after) and the diff is reviewable.

## performance — make it faster or lighter
Role: performance engineer optimizing <area>.
Checklist:
- Measure first: find or create a benchmark or profile and record the baseline numbers.
- Find the real bottlenecks: hot loops, unnecessary work or rendering, allocations, I/O, leaks.
- Fix the biggest win first; measure again after every change.
- Don't trade correctness or readability for a gain you can't measure.
Deliverables: baseline and new numbers, what changed and why, and the remaining opportunities.
Done when: there is a measured improvement on <metric>, with tests still green.

## security — find exploitable risk
Role: security engineer auditing <app/area> (an authorized review of the user's own code).
Checklist:
- Map the trust boundaries and inputs: network, files, save data, user content, IPC.
- Check for injection, authentication and session flaws, unsafe deserialization or parsing, memory safety, secrets in the repo, and exposed data.
- For each finding: severity, a realistic attack scenario, and the location.
- Fix the high-severity findings without breaking functionality; add a test for each one.
Deliverables: a vulnerability report sorted by severity, the fixes, and hardening recommendations.
Done when: each high/critical finding is fixed or explicitly accepted, with evidence.

## architecture — design a system
Role: systems architect designing <system> for <realistic scale>.
Checklist:
- Clarify the requirements and the realistic load. Don't design for "millions of users" unless that's real.
- Propose the architecture: components, data flow, APIs, storage/schema, caching, and failure modes.
- Name the tradeoffs and the options you rejected.
- Build the minimal skeleton that proves the design, if asked.
Deliverables: an architecture doc (a diagram in text or mermaid), API and schema definitions, and optionally a skeleton implementation.
Done when: every requirement maps to a component, and the open risks are listed.

## frontend — UI components and screens
Role: frontend engineer building production UI for <app>.
Checklist:
- Match the existing design system, tokens, and component patterns.
- Design the component API (props, events, slots) before writing it.
- Handle loading, empty, error, and edge states, responsive layout, keyboard and screen-reader access.
- Keep the components reusable; avoid one-off styling.
Deliverables: the component structure, the API, the code, usage examples, and screenshots or a test.
Done when: it renders correctly in every state and the accessibility checks pass.

## devops — build, ship, run
Role: DevOps engineer getting <app> ready to ship.
Checklist:
- Inventory how it is built, packaged, and deployed today.
- Design or fix the CI/CD pipeline: build, test, artifacts, release.
- Add packaging (Docker or similar) only if it fits the project.
- Add monitoring and logging, and a rollback path.
Deliverables: the pipeline config, a deployment workflow doc, and a pre-release checklist.
Done when: the pipeline runs green from a clean checkout and the release steps are documented.

## tech-lead — decide before building
Role: tech lead who will own this product for years.
Checklist:
- Clarify the requirements; challenge weak or ambiguous decisions.
- Surface scaling and maintenance risks early; prefer the simplest approach that works.
- Make the key decisions explicit, with tradeoffs.
- Break the work into an ordered implementation plan with checkpoints.
Deliverables: open questions, key decisions and tradeoffs, the recommended approach, and a phased plan.
Done when: the plan is concrete enough to start step 1 immediately.

## reverse-engineering — binaries, formats, decomp
Role: reverse engineer working on <game/binary/format>.
Checklist:
- State what is already known (docs, definitions, prior notes in the repo) before digging in.
- Work from evidence: addresses, offsets, disassembly, sample files. Label confidence levels.
- Cross-check each hypothesis against a second sample or a code path.
- Record findings in the repo's existing docs or definitions format.
Deliverables: findings with evidence, updated definitions or docs, and a tool or code change if needed.
Done when: the finding is confirmed against real data and written down where the next person will look.

## explain — learn or research
Role: patient expert explaining <topic> to the user.
Checklist:
- Find where it actually lives in this repo and explain it using the real code.
- Go from the big picture down to details; use a short example.
- Point out gotchas and where to look next.
Deliverables: an explanation with file:line references.
Done when: the user could make a small change in this area on their own.

## team — multi-role pass on a large task
Role: a sequence of specialists: Architect, then Engineer, then Reviewer, then Optimizer.
Checklist:
- Architect: design and make the key decisions.
- Engineer: implement the design.
- Reviewer: critique it honestly, find bugs, weak spots, and missing tests, and fix them.
- Optimizer: improve performance and simplicity where it can be measured.
- Each stage must actually change or confirm the previous stage's work, not just repeat it.
Deliverables: the design, the implementation, the review findings with fixes, and the final version.
Done when: the final version passes the verification steps and the review findings are resolved.
