#!/bin/bash

for X in `find  -type d | grep -v "\/\." | cut -d'/' -f2 | grep -v "\."` ; do cat SConsTemplate | sed "s/__MC__/${X}/g" > ${X}/SConscript; done

