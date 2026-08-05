# Security Policy

## Supported Versions

Security fixes are developed on `main` and released in the current version of
Meccha Camouflage. Older releases may not receive fixes.

## Reporting a Vulnerability

Please do not report a suspected vulnerability in a public issue, discussion,
or pull request. Use GitHub's private vulnerability-report form instead:

<https://github.com/acentrist/MecchaCamouflage/security/advisories/new>

If private reporting is unavailable, contact a repository maintainer privately
through GitHub. Include:

- the affected version or commit and whether it is a release or source build;
- a minimal, safe reproduction and the expected versus actual result;
- the potential impact and any relevant, redacted diagnostic output; and
- enough environment detail to reproduce the issue, such as Windows version.

Do not include account credentials, authentication or bridge tokens, personal
data, or unredacted logs in a report.

## Scope

Reports are in scope when they affect this repository's source code or official
release artifact, including the desktop host, packaged WebView UI, native
bridge, injector, and runtime-file staging.

False-positive antivirus detections, compatibility problems, and ordinary bugs
should be reported through the normal issue tracker unless they demonstrate a
security impact.

## Handling and Disclosure

Maintainers will assess the report, work with the reporter on a fix when
applicable, and coordinate public disclosure after users have a corrected
release. Please give maintainers a reasonable opportunity to investigate and
ship a fix before publishing technical details.
