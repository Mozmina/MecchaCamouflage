#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PS_SCRIPT="$SCRIPT_DIR/dev_flow.ps1"

if command -v pwsh >/dev/null 2>&1; then
  pwsh -NoProfile -ExecutionPolicy Bypass -File "$PS_SCRIPT" "$@"
  exit $?
fi

if ! command -v powershell.exe >/dev/null 2>&1; then
  echo "PowerShell runtime not found. Install pwsh or ensure powershell.exe is available." >&2
  exit 127
fi

if command -v wslpath >/dev/null 2>&1; then
  PS_SCRIPT_WIN="$(wslpath -w "$PS_SCRIPT")"
else
  PS_SCRIPT_WIN="$PS_SCRIPT"
fi

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PS_SCRIPT_WIN" "$@"
