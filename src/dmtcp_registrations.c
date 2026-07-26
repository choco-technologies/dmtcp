/*
 * Registers dmtcp's Built-in API in the .dmod.inputs section - the same
 * shape dmlist_registrations.c/dmosi_registrations.c/dmlog_registrations.c
 * use for their own multi-file modules.
 *
 * This must live in its own translation unit, separate from every other
 * dmtcp_*.c file: the registration struct array dmtcp_defs.h generates
 * when DMOD_ENABLE_REGISTRATION is set covers every function declared in
 * dmtcp.h, not just the ones defined in whichever file set the macro - so
 * defining it in more than one translation unit produces one duplicate
 * "multiple definition" linker error per public function. Only dmtcp.h is
 * included here (not the full dmod.h) so this can't accidentally
 * re-register dmod's own kernel Built-in APIs too - see
 * dmlist_registrations.c's own doc comment for the same reasoning.
 */
#define DMOD_ENABLE_REGISTRATION ON
#include "dmtcp.h"
