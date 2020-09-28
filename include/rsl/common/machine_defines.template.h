#pragma once

// This file defines the maximum number of threads to use in template constants,
// to speed up compilation on systems that don't need higher thread counts.

// To build this project, you must copy this file to "machine_defines.h,"
// and set MAX_THREADS appropriately for your local machine.

#define MAX_THREADS 4
