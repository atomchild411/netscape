/* -*- Mode: C; tab-width: 4 -*-
 *   hk_none.c --- hooks without JavaScript (NS_QUICKJS).
 *
 * Hooks were JavaScript functions, read from a hook file, that the 1998
 * engine called at points in the browser (unknown tags, location
 * changes).  With JavaScript on QuickJS there are no hooks.
 */

#include "xp_core.h"
#include "hk_funcs.h"

const char *
HK_GetFunctionName(int32 hook_id, void *extra)
{
	return NULL;
}

intn
HK_Init(void)
{
	return 1;
}

intn
HK_IsHook(int32 hook_id, void *extra)
{
	return 0;
}

intn
HK_CallHook(int32 hook_id, void *extra, int32 window_id,
			char *hook_str, char **hook_ret)
{
	if (hook_ret)
		*hook_ret = NULL;
	return 0;
}

void
HK_ReadHookFile(char *filename)
{
}
