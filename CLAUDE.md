# CLAUDE.md

This file contains rules, responsibilities, and best practices for all agents
working in this repository. Implementation details, setup guides, and design
documentation live in the agent-owned documents listed below.

---

## Agent responsibilities and owned documents

| Agent | Owns | Maintained document |
|-------|------|---------------------|
| **Product Manager** | Requirements, product vision, scope decisions | — |
| **Firmware** | `firmware/` — FreeRTOS tasks, ESP8266 SDK, transport layer, NVS, build system | `FIRMWARE.md` |
| **Hardware** | GPIO wiring, voltage levels, sensor selection, circuit design | `HARDWARE.md` |
| **Server** | `server/` — FastAPI, SQLite schema, Pydantic models, SSE, deployment | `SERVER.md` |

Each agent **must** keep their document current: any commit that changes
behaviour described in that document must update the document in the same commit.

---

## Requirement triage workflow (every new requirement)

No implementation starts before this cycle is complete.

1. **PM states the requirement** — does not need to be fully specified.
2. **All three specialists triage in parallel** — each produces an impact
   assessment, risks/constraints, and specific clarifying questions for the PM.
3. **PM answers** — may revise or narrow the requirement.
4. **Repeat** until all specialists confirm they have enough to implement correctly.
5. **Implementation plan** — specialists agree on which files change, which
   contracts need updating, and the commit strategy.
6. **Implementation** — contract documents updated in the same commit as the code.

---

## Cross-boundary rules

### Hardware ↔ Firmware

`HARDWARE.md` is the single source of truth for all physical board configuration.

- **Hardware Agent changing anything** → update `HARDWARE.md` first, then
  notify Firmware Agent so firmware constants are updated in the same commit.
- **Firmware Agent needing a hardware change** → raise it with Hardware Agent
  first; do not assume wiring or pin changes are free.

### Firmware ↔ Server (wire format)

`API_CONTRACT.md` is jointly owned by Firmware and Server agents.

- Neither agent may change the JSON format, endpoint, field types, or batch
  behaviour unilaterally.
- Either agent proposing a change must get explicit agreement from the other first.
- Agreed changes land in **one atomic commit** covering `API_CONTRACT.md`,
  firmware, and server — the repo must never be in a state where the two sides
  are incompatible.
- Even non-breaking additions require a change-log entry in `API_CONTRACT.md`.

### Firmware — NVS storage

The NVS layout is documented in `FIRMWARE.md` (§ NVS Storage).

- Any commit that adds, removes, renames, or changes the type of an NVS
  namespace or key **must** update the NVS Storage section of `FIRMWARE.md`
  and its change log in the same commit.
- Document the migration strategy for any key rename or type change — stale
  keys are not erased automatically on firmware update.

---

## Deferred work

`DEFERRED.md` records items explicitly scoped out during triage, with full
reasoning and prerequisites. Consult it before starting any work that touches
cellular transport, deep sleep, or per-transport POST intervals.
