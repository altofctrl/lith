#!/bin/bash
# lith knapp backend: Flask on 127.0.0.1:3130 (proxied at lith.vidalion.co/api/)
cd "$(dirname "$0")"
export PATH="$HOME/.local/bin:$PATH"
exec ./venv/bin/python app.py
