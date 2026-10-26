#!/usr/bin/env python3
"""Appends a footer file to a generated unit: usage: append.py <footer> <unit>"""

import sys

with open(sys.argv[1], encoding="utf-8") as footer, open(sys.argv[2], "a", encoding="utf-8") as unit:
    unit.write("\n// Appended for the web build by port/web/append.py.\n")
    unit.write(footer.read())
