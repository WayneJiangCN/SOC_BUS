#ifndef _TM_MEM_ALIAS_H_
#define _TM_MEM_ALIAS_H_

/*
 * Compatibility wrapper.
 *
 * Most aicore sources include "tm_mem.h", while the current repository keeps
 * the implementation header as "pem_mem.h". Keeping this tiny shim avoids
 * touching all existing includes and gives new top-level tests a stable name.
 */
#include "pem_mem.h"

#endif  // _TM_MEM_ALIAS_H_
