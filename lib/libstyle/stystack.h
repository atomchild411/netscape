/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 *
 * The contents of this file are subject to the Netscape Public License
 * Version 1.0 (the "NPL"); you may not use this file except in
 * compliance with the NPL.  You may obtain a copy of the NPL at
 * http://www.mozilla.org/NPL/
 *
 * Software distributed under the NPL is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
 * for the specific language governing rights and limitations under the
 * NPL.
 *
 * The Initial Developer of this code under the NPL is Netscape
 * Communications Corporation.  Portions created by Netscape are
 * Copyright (C) 1998 Netscape Communications Corporation.  All Rights
 * Reserved.
 */

#ifndef NSPR20
#include "prhash.h"
#else
#include "plhash.h"
#endif

#ifndef SML_HEADER
#define SML_HEADER

#define STYLESTACK_Init(self, context)				(*self->vtable->Init)(self,context)
#define STYLESTACK_PopTag(self,name)				(*self->vtable->PopTag)(self,name)
#define STYLESTACK_PopTagByIndex(self,name, index)		(*self->vtable->PopTagByIndex)(self,name,index)
#define STYLESTACK_PushTag(self,name,class_name,id)		(*self->vtable->PushTag)(self,name,class_name,id)
#define STYLESTACK_GetTagByIndex(self, i)			(*self->vtable->GetTagByIndex)(self, i)
#define STYLESTACK_GetTagByReverseIndex(self, i)	(*self->vtable->GetTagByReverseIndex)(self, i)
#define STYLESTACK_GetStyleByIndex(self, i)			(*self->vtable->GetStyleByIndex)(self, i)
#define STYLESTACK_GetStyleByReverseIndex(self, i)	(*self->vtable->GetStyleByReverseIndex)(self, i)
#define STYLESTACK_FreeTagStruct(self,tag)			(*self->vtable->FreeTagStruct)(self,tag)
#define STYLESTACK_NewTagStruct(self,name,class_name,id)	(*self->vtable->NewTagStruct)(self,name,class_name,id)
#define STYLESTACK_Delete(self)						(*self->vtable->Delete)(self)
#define STYLESTACK_Purge(self)						(*self->vtable->Purge)(self)
#define STYLESTACK_SetSaveOn(self, i)					(*self->vtable->SetSaveOn)(self, i)
#define STYLESTACK_IsSaveOn(self)					(*self->vtable->IsSaveOn)(self)




typedef struct {

    char *name;
    char *class_name;
    char *id;
    void *css_node;     /* NSCSS_Node, with NS_LIBCSS */

} TagStruct;

typedef enum {
PUSH_TAG_ERROR,
PUSH_TAG_SUCCESS,
PUSH_TAG_BLOCKED
} PushTagStatus;

typedef struct _StyleAndTagStackInterface StyleAndTagStackInterface;

typedef struct {
    StyleAndTagStackInterface  *vtable;
    int32                       refcount;
} StyleAndTagStack;

struct _StyleAndTagStackInterface {
	void 				(*Init)
			(StyleAndTagStack *self, MWContext *context);
	TagStruct * 		(*NewTagStruct)
			(StyleAndTagStack *self, char *name, char *class_name, char *id);
	void 				(*FreeTagStruct)
			(StyleAndTagStack *self, TagStruct *tag);
	PushTagStatus 		(*PushTag)
			(StyleAndTagStack *self, char *name, char *class_name, char *id);
	void  				(*PopTag)
			(StyleAndTagStack *self, char *name);
	void  				(*PopTagByIndex)
			(StyleAndTagStack *self, char *name, int32 index);
	TagStruct * 		(*GetTagByIndex)	
			(StyleAndTagStack *self, int32 index);
	TagStruct * 		(*GetTagByReverseIndex)	
			(StyleAndTagStack *self, int32 index);
	StyleStruct * 		(*GetStyleByIndex)	
			(StyleAndTagStack *self, int32 index);
	StyleStruct * 		(*GetStyleByReverseIndex)	
			(StyleAndTagStack *self, int32 index);
	StyleStruct * 		(*Delete)	
			(StyleAndTagStack *self);
	StyleStruct * 		(*Purge)	
			(StyleAndTagStack *self);
	void 				(*SetSaveOn)	
			(StyleAndTagStack *self, XP_Bool on_flag);
	XP_Bool				(*IsSaveOn)	
			(StyleAndTagStack *self);
};


/* initializer */
extern StyleAndTagStack * SML_StyleStack_Factory_Create(void);

typedef enum JSSObjectTypeEnum {
	JSSTags = 1, /* document.tags */
	JSSIds,   	 /* document.ids */
	JSSClasses,  /* document.classes */
	JSSClass	 /* document.classes.<some class> */
} JSSObjectType;

typedef struct _StyleObject {
	PRHashTable	   *table;
	JSSObjectType	type;
	char		   *name;  /* only used when type is JSSClass */
} StyleObject;

extern void
SML_SetObjectRefs(StyleAndTagStack *styleStack,
				  StyleObject	   *tags,
				  StyleObject	   *classes,
				  StyleObject	   *ids);

/* Helper routine to assemble the JSSContext */
struct JSSContext;

extern void
sml_GetJSSContext(StyleAndTagStack *styleStack, struct JSSContext *jc);

/* With NS_LIBCSS (see nscss.h): CSS goes to libcss, not JavaScript. */

/* Is CSS handled by libcss in this build? */
extern XP_Bool SML_UsesLibCSS(StyleAndTagStack *styleStack);

/* PushTag, with the tag's attribute text (tag->data) for attribute
 * selectors, its style attribute, and the base URL for url()s in it. */
extern PushTagStatus
SML_PushTagWithAttrs(StyleAndTagStack *styleStack, char *name,
                     char *class_name, char *id, const char *attrs,
                     int32 attrs_len, const char *style, const char *base_url);

/* Add an author style sheet (see NSCSS_AddSheet). */
extern void
SML_AddStyleSheet(StyleAndTagStack *styleStack, const char *url,
                  const char *charset, const char *media,
                  const char *data, int32 len);

/* The window size, for media queries. */
extern void
SML_SetViewport(StyleAndTagStack *styleStack, int32 width, int32 height);

#endif /* SML_HEADER */
