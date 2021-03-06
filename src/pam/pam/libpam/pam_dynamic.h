#ifndef _PAM_DYNAMIC_H
#define _PAM_DYNAMIC_H

#include "pam_private.h"

void *_pam_dlopen(const char *mod_path);
servicefn _pam_dlsym(void *handle, const char *symbol);
void _pam_dlclose(void *handle);

#endif
