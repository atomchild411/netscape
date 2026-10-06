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

#include "xp.h"
#include "plstr.h"
#include "prmem.h"
#include "net.h"
#include "netutils.h"
#include "mkselect.h"
#include "mktcp.h"
#include "mkgeturl.h"
#include "mkstream.h"
#include "dataurl.h"
#include "cvmime.h"

extern int MK_OUT_OF_MEMORY;
extern int MK_MALFORMED_URL_ERROR;

/* Decode base64 TEXT (LEN bytes; white space and anything else that is
 * not base64 skipped) in place; returns the decoded length. */
PRIVATE int32
net_data_url_unbase64(char *text, int32 len)
{
	int32 i, out = 0, bits = 0, nbits = 0;

	for (i = 0; i < len; i++) {
		int c = (unsigned char) text[i], v;

		if (c >= 'A' && c <= 'Z') v = c - 'A';
		else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
		else if (c >= '0' && c <= '9') v = c - '0' + 52;
		else if (c == '+' || c == '-') v = 62;
		else if (c == '/' || c == '_') v = 63;
		else if (c == '=') break;
		else continue;
		bits = (bits << 6) | v;
		nbits += 6;
		if (nbits >= 8) {
			nbits -= 8;
			text[out++] = (char) ((bits >> nbits) & 0xff);
		}
	}
	return out;
}

/* format of the DATA: URL
 *
 * data:[CONTENT-TYPE][;base64],DATA
 */
PRIVATE int32
net_DataURLLoad (ActiveEntry * ce)
{
	char *data_buffer;
	XP_Bool is_base64 = FALSE;
	NET_StreamClass *stream;
	char *comma;
	char *preset = NULL;

    ce->protocol = DATA_TYPE_URL;

	/* we need a buffer equal to or smaller than the size of the URL
	 */
	data_buffer = (char *)PR_Malloc(PL_strlen(ce->URL_s->address)+1);

	if(!data_buffer)
		return(MK_OUT_OF_MEMORY);

	/* determine the content type */

	/* find the first comma */
	comma = PL_strchr(ce->URL_s->address, ',');

	/* if no comma abort */
	if(!comma)
	{
		ce->URL_s->error_msg = NET_ExplainErrorDetails(MK_MALFORMED_URL_ERROR, ce->URL_s->address);
		return(MK_MALFORMED_URL_ERROR);
	}

	/* A type the caller preset (a <script src> or style sheet load: its
	 * converter goes by that type) stays: the data's own type would send
	 * a script to the HTML parser, which then replaced the page. */
	if(ce->URL_s->preset_content_type && ce->URL_s->content_type &&
	   *ce->URL_s->content_type)
		preset = PL_strdup(ce->URL_s->content_type);

	/* fill in default content type */
	StrAllocCopy(ce->URL_s->content_type, TEXT_PLAIN);

	/* check for a content type */
	if(comma != ce->URL_s->address + PL_strlen("data:"))
	{
		*comma = '\0';
		PL_strcpy(data_buffer, ce->URL_s->address + PL_strlen("data:"));		
		*comma = ',';

		/* check for base 64 encoding */
		if(PL_strcasestr(data_buffer, "base64"))
			is_base64 = TRUE;

		/* parse the rest as a content-type */
		NET_ParseContentTypeHeader(ce->window_id, data_buffer, ce->URL_s, FALSE);

	}
	if(preset)
	{
		StrAllocCopy(ce->URL_s->content_type, preset);
		PL_strfree(preset);
	}

	/* open the outgoing stream (base64 is decoded here: the base64
	 * converter is not there for every format, images' for one) */
	stream = NET_StreamBuilder(ce->format_out, ce->URL_s, ce->window_id);

	if(!stream)
	  {
		ce->URL_s->error_msg = NET_ExplainErrorDetails(MK_UNABLE_TO_CONVERT);
		ce->status = MK_UNABLE_TO_CONVERT;
		return (ce->status);
	  }

	/* @@@@ bug: ignore is_write_ready */

	/* copy the data part of the URL into a scratch buffer */
	PL_strcpy(data_buffer, comma+1);

    /* the data is %-escaped (base64 data too, before decoding) */
    {
        int32 len = NET_UnEscapeCnt(data_buffer);

        if(is_base64)
            len = net_data_url_unbase64(data_buffer, len);
        ce->status = (*stream->put_block)(stream, data_buffer, len);
    }
    if(ce->status < 0)
      {
    	(*stream->abort)(stream, ce->status);
        return (ce->status);
      }

    (*stream->complete)(stream);

	ce->status = MK_DATA_LOADED;
    return(-1); /* all done */
	
}

/* called repeatedly from NET_ProcessNet to push all the
 * data up the stream
 */
PRIVATE int32
net_ProcessDataURL (ActiveEntry * cur_entry)
{
	PR_ASSERT(0);
	return(-1);
}

/* called by functions in mkgeturl to interrupt the loading of
 * an object.  (Usually a user interrupt) 
 */
PRIVATE int32
net_InterruptDataURL (ActiveEntry * cur_entry)
{
	PR_ASSERT(0);
	return(-1);
}

PRIVATE void
net_CleanupDataURL(void)
{

}

MODULE_PRIVATE void
NET_InitDataURLProtocol(void)
{
        static NET_ProtoImpl dataurl_proto_impl;

        dataurl_proto_impl.init = net_DataURLLoad;
        dataurl_proto_impl.process = net_ProcessDataURL;
        dataurl_proto_impl.interrupt = net_InterruptDataURL;
        dataurl_proto_impl.cleanup = net_CleanupDataURL;

        NET_RegisterProtocolImplementation(&dataurl_proto_impl, DATA_TYPE_URL);
}

