#ifndef HV_CLI_H
#define HV_CLI_H

/* Parse W,H as powers of 2 from 2 to 32,768, returning their exponents.
 * 0 on success, -1 on invalid input. */
int hv_cli_precincts(const char *value, int *ppx, int *ppy);

#endif
