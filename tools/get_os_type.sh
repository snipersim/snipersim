#!/bin/bash

OS_TYPE="Unknown"
OS_VERSION="Unknown"

grep -q "Scientific Linux" /etc/issue >/dev/null 2>&1
RES="$?"
if [ "$RES" -eq 0 ]; then
	OS_TYPE="ScientificLinux"
fi

grep -q "CentOS" /etc/issue >/dev/null 2>&1
RES="$?"
if [ "$RES" -eq 0 ]; then
	OS_TYPE="ScientificLinux"
fi

grep -q "Ubuntu" /etc/issue >/dev/null 2>&1
RES="$?"
if [ "$RES" -eq 0 ]; then
	OS_TYPE="Ubuntu"
fi

grep -q "Debian" /etc/issue >/dev/null 2>&1
RES="$?"
if [ "$RES" -eq 0 ]; then
	OS_TYPE="Debian"
fi

echo "${OS_TYPE}"
