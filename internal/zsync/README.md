# zsync

## Overview

The zsync module library implements the higher level parts of the process that the
zsync client uses for reconstructing a target file from local data plus downloaded
data.

This is a Go port of the zsync library from the C version of zsync.

## Copyright Notice

Copyright © 2004,2005,2007,2009,2025,2026 Colin Phipps <cph@moria.org.uk>

This module is free software; you can redistribute it and/or modify it under the
terms of the Artistic License v2 (see the accompanying file COPYING for the full
license terms), or, at your option, any later version of the same license.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the COPYING file for details.

## Features

- zsync control file parsing to set up initial state for a file transfer.
- thin wrapper around the file reconstruction methods from the rcksum library.
- verification of the download on completion using the metadata from the control
  file.
