# Security

Report a vulnerability privately, through [GitHub's vulnerability reporting](https://github.com/dresden-elektronik/phoscon-rcp/security/advisories/new), not in a public issue. Name the board, the release its `NCP_VERSION` reports, and how to reproduce the problem.

Fixes go into the latest release only.

This firmware runs on the adapter and handles keys only as a host sets them: it keeps none across a reset and writes no network data to flash. A problem in a host or application belongs to that project.
