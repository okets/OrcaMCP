# Architecture Decision Records (ADRs)

This directory contains Architecture Decision Records documenting key technical decisions made during OrcaMCP development.

## What is an ADR?

An ADR is a document that captures an important architectural decision along with its context and consequences. ADRs help future contributors understand *why* decisions were made.

## ADR Index

| ID | Title | Status |
|----|-------|--------|
| [0001](0001-http-transport.md) | HTTP Transport with stdio Bridge | Accepted |
| [0002](0002-embedded-server.md) | Embedded Server in OrcaSlicer | Accepted |
| [0003](0003-json-rpc-protocol.md) | JSON-RPC 2.0 Protocol | Accepted |
| [0004](0004-main-thread-execution.md) | Main Thread Execution | Accepted |

## ADR Template

When making a significant architectural decision, create a new ADR using this template:

```markdown
# ADR-NNNN: Title

## Status
[Proposed | Accepted | Deprecated | Superseded by ADR-XXXX]

## Context
What is the issue that we're seeing that is motivating this decision?

## Decision
What is the change that we're proposing and/or doing?

## Consequences
What becomes easier or more difficult to do because of this change?
```

## When to Write an ADR

Write an ADR when:
- Adding a new major component
- Changing how components communicate
- Selecting a significant library or framework
- Making a decision that affects multiple areas of the codebase
- Making a decision that's hard to reverse

## Naming Convention

ADRs are numbered sequentially: `NNNN-short-title.md`

Examples:
- `0001-http-transport.md`
- `0002-embedded-server.md`
