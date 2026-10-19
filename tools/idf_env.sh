#!/usr/bin/env bash
# Source this file. All downloaded tools stay outside the project and system.
_katmed_tools="${KATMED_TOOLS_DIR:-${HOME}/.local/share/katmed-tools}"
export IDF_PATH="${_katmed_tools}/esp-idf-v6.1-acme-20261019"
export IDF_TOOLS_PATH="${_katmed_tools}/esp-idf-v6.1-tools-acme-20261019"
source "${IDF_PATH}/export.sh" >/dev/null
export PATH="${PATH}:${_katmed_tools}/bootstrap/bin"
unset _katmed_tools
