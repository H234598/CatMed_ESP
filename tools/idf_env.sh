#!/usr/bin/env bash
# Source this file. All downloaded tools stay outside the project and system.
_katmed_tools="${KATMED_TOOLS_DIR:-${HOME}/.local/share/katmed-tools}"
export IDF_PATH="${_katmed_tools}/esp-idf-v5.5.5"
export IDF_TOOLS_PATH="${_katmed_tools}/idf-tools"
export IDF_PYTHON_ENV_PATH="${IDF_TOOLS_PATH}/python_env/idf5.5_py3.13_env"
export PATH="${IDF_PYTHON_ENV_PATH}/bin:${_katmed_tools}/bootstrap/bin:${IDF_TOOLS_PATH}/tools/riscv32-esp-elf/esp-14.2.0_20260121/riscv32-esp-elf/bin:${IDF_PATH}/tools:${PATH}"
unset _katmed_tools
