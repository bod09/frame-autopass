#!/bin/bash
# Short address for the frame-autopass installer:
#   curl -L bod09.github.io/frame-autopass/i | bash
# Runs the installer from the latest release.
set -euo pipefail
curl -fsSL https://github.com/bod09/frame-autopass/releases/latest/download/install.sh | bash
