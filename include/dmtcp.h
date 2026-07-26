#ifndef DMTCP_H
#define DMTCP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmod_types.h"
#include "dmtcp_defs.h"

/**
 * Public API for the dmtcp module.
 *
 * Functions are declared with the dmod_dmtcp_api(...) macro - dmod's
 * standard pattern for functions callable from other modules (or from this
 * module's own tests/), resolved dynamically by the loader rather than
 * through normal static linkage. See dm_sw_ring/include/dm_sw_ring.h for a
 * fully worked real-world example of the same shape.
 *
 * Definitions in src/dmtcp.c use the matching
 * dmod_dmtcp_api_declaration(...) macro - a plain C function
 * definition here will NOT satisfy these declarations at link time.
 *
 * This is an example interface using the usual "opaque handle" pattern -
 * replace the handle, functions, and struct definition in
 * src/dmtcp.c with your module's real API.
 */

/* Opaque handle - the real struct is defined in src/dmtcp.c */
typedef struct dmtcp* dmtcp_t;

/**
 * Create a new dmtcp instance.
 *
 * @return A valid handle on success, or NULL on allocation failure.
 */
dmod_dmtcp_api(1.0, dmtcp_t, _create, ( void ));

/**
 * Destroy an instance created by dmtcp_create(). Safe to call with
 * NULL.
 */
dmod_dmtcp_api(1.0, void, _destroy, ( dmtcp_t handle ));

/**
 * Example accessor - replace with your module's real API.
 *
 * @return true if handle is a valid, non-NULL instance.
 */
dmod_dmtcp_api(1.0, bool, _is_valid, ( dmtcp_t handle ));

#endif // DMTCP_H
