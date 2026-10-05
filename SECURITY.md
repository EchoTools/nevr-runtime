# Security Policy

## Supported Versions

Only the **latest release** gets security fixes. Older releases and unreleased builds are not supported. If you're on
anything else, upgrade to the latest release from this repository's GitHub Releases page first.

| Version          | Supported          |
| ---------------- | ------------------ |
| Latest release   | :white_check_mark: |
| Anything older   | :x:                |

## Reporting a Vulnerability

**Please don't report security problems in public issues, discussions, or Discord.**

Report privately through GitHub: open this repository's **Security** tab and choose **Report a vulnerability**. Include:

- the release tag or commit you tested
- what you did, what happened, and what you expected
- the impact, as you understand it
- a proof of concept, if you have one

## What to Expect

- **Acknowledgement within 7 days.** This is a small volunteer project, so please be patient.
- **A status update at least every 14 days** until the report is resolved.
- **If we accept it:** we fix it in a new release and publish an advisory crediting you, unless you'd rather not be
  credited.
- **If we decline it:** we tell you why.

## Scope

**In scope:** the code in this repository (the runtime DLLs, plugins and tooling), how it talks to the game service
(connection handling, authentication tokens), and the release artifacts published from it.

**Out of scope, or report elsewhere:**

- The game service itself: report privately to the EchoTools/nakama repository.
- The original Echo VR game code.
- Community-run game servers.
- The HTTP and server keys built into release binaries. They are public by design, not leaked secrets.

## Please Don't

- Test against other players' accounts, or against servers you don't run. Use a local game service instead.
- Run denial-of-service tests.
