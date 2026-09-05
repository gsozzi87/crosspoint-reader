#!/usr/bin/env bash
# Setup script del environment de Claude Code web (se pega en la configuración del entorno).
set -e
git submodule update --init --recursive
pip install -q platformio
# Primer build para cachear la toolchain (opcional, tarda varios minutos la primera vez)
# python3 -m platformio run -e ws397 || true
