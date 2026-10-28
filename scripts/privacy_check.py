#!/usr/bin/env python3
"""Privacy check before publishing: searches the working tree AND the whole git history.

Looks at file contents of every revision, commit messages and author/committer addresses.
Generic findings: e-mail addresses (except *noreply* ones), home directories, private keys, tokens.
Your own identifiers (home network, domains, names) do not belong in this public file — put them, one
regular expression per line, into a pattern file outside the repository:

    ~/.config/platinenlupe/privacy-patterns.txt      (or set PL_PRIVACY_PATTERNS=/path/to/file)

  python3 scripts/privacy_check.py            exit 0 = clean, 1 = findings
  python3 scripts/privacy_check.py --tree     working tree only (fast)
"""

import os
import pathlib
import re
import subprocess
import sys

GENERIC = [
    (r"[A-Za-z0-9._%+-]+@(?![A-Za-z0-9.-]*noreply)[A-Za-z0-9.-]+\.[a-z]{2,}", "e-mail address"),
    (r"/home/[a-z][a-z0-9_-]+/", "home directory"),
    (r"/Users/[A-Za-z][A-Za-z0-9_-]+/", "home directory (macOS)"),
    (r"-----BEGIN [A-Z ]*PRIVATE KEY-----", "private key"),
    (r"\b(ghp|gho|github_pat)_[A-Za-z0-9_]{20,}", "GitHub token"),
    (r"\bsk-[A-Za-z0-9]{20,}", "API key"),
]
# Addresses that are part of the product, not of anyone's home.
ALLOWED = [r"noreply@anthropic\.com", r"[A-Za-z0-9._-]+@users\.noreply\.github\.com"]
SKIP_SUFFIX = (".stl", ".3mf", ".png", ".gif", ".jpg", ".bin")


def load_patterns() -> list[tuple[str, str]]:
    path = pathlib.Path(os.environ.get("PL_PRIVACY_PATTERNS", "~/.config/platinenlupe/privacy-patterns.txt")).expanduser()
    own = []
    if path.exists():
        own = [(line.strip(), "own pattern") for line in path.read_text().splitlines()
               if line.strip() and not line.startswith("#")]
    else:
        print(f"note: no own pattern file at {path} — only generic checks", file=sys.stderr)
    return GENERIC + own


def git(*args: str) -> str:
    return subprocess.run(["git", *args], capture_output=True, text=True, errors="replace", check=True).stdout


def scan(text: str, where: str, patterns, findings: list) -> None:
    for regex, kind in patterns:
        for m in re.finditer(regex, text, re.IGNORECASE):
            if any(re.fullmatch(a, m.group(0), re.IGNORECASE) for a in ALLOWED):
                continue
            findings.append(f"{where}: {kind}: {m.group(0)[:60]}")


def main() -> int:
    patterns = load_patterns()
    findings: list[str] = []
    for f in git("ls-files").splitlines():
        if f.lower().endswith(SKIP_SUFFIX) or f == "scripts/privacy_check.py":
            continue
        try:
            scan(pathlib.Path(f).read_text(errors="replace"), f, patterns, findings)
        except (IsADirectoryError, FileNotFoundError):
            pass
    if "--tree" not in sys.argv:
        scan(git("log", "--all", "--format=%H %an <%ae> %cn <%ce>%n%B"), "commit metadata/messages", patterns, findings)
        for rev in git("rev-list", "--all").split():
            for f in git("ls-tree", "-r", "--name-only", rev).splitlines():
                if f.lower().endswith(SKIP_SUFFIX) or f == "scripts/privacy_check.py":
                    continue
                scan(git("show", f"{rev}:{f}"), f"{rev[:8]}:{f}", patterns, findings)
    unique = sorted(set(findings))
    for line in unique[:200]:
        print(line)
    print(f"{len(unique)} finding(s)" + (" — first 200 shown" if len(unique) > 200 else ""))
    return 1 if unique else 0


if __name__ == "__main__":
    sys.exit(main())
