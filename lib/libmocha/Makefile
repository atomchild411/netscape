#!gmake
#
# The contents of this file are subject to the Netscape Public License
# Version 1.0 (the "NPL"); you may not use this file except in
# compliance with the NPL.  You may obtain a copy of the NPL at
# http://www.mozilla.org/NPL/
#
# Software distributed under the NPL is distributed on an "AS IS" basis,
# WITHOUT WARRANTY OF ANY KIND, either express or implied. See the NPL
# for the specific language governing rights and limitations under the
# NPL.
#
# The Initial Developer of this code under the NPL is Netscape
# Communications Corporation.  Portions created by Netscape are
# Copyright (C) 1998 Netscape Communications Corporation.  All Rights
# Reserved.



DEPTH		= ../..

MODULE		= mocha
LIBRARY_NAME	= mocha

REQUIRES	= lay parse img js style layer applet dbm nspr security \
		  htmldlgs util jtools pref java libreg softupdt jsdebug netcache network httpurl

ifdef MOZ_SECURITY
REQUIRES       += jar
endif

ifdef NS_QUICKJS
# JavaScript on QuickJS (qjs.h): the 1998 engine's files are not built.
REQUIRES	= lay parse img style layer dbm nspr htmldlgs util pref netcache \
		  network httpurl
CSRCS		= qjs_core.c \
		  qjs_dom.c \
		  qjs_net.c \
		  qjs_window.c \
		  $(NULL)
else
CSRCS		= et_mocha.c \
		  et_moz.c \
		  lm_applt.c \
		  lm_bars.c \
		  lm_cmpnt.c \
		  lm_doc.c \
		  lm_embed.c \
		  lm_event.c \
		  lm_form.c \
		  lm_hardw.c \
		  lm_hist.c \
		  lm_href.c \
		  lm_img.c \
		  lm_init.c \
		  lm_input.c \
		  lm_nav.c \
		  lm_plgin.c \
		  lm_screen.c \
		  lm_span.c \
		  lm_supdt.c \
		  lm_taint.c \
                  lm_trans.c \
		  lm_trggr.c \
		  lm_url.c \
		  lm_win.c \
		  lm_layer.c \
		  $(NULL)
endif


ifdef MOZ_JAVA
CSRCS +=             \
		  lm_jsd.c \
		  $(NULL)
endif

include $(DEPTH)/config/rules.mk

ifdef NS_QUICKJS
# the DOM's JavaScript, built in (mkjsheader.py qjs_dom.js)
$(OBJDIR)/qjs_dom.o: qjs_dom_js.h
endif

DEFINES		+= -DDLL_SUFFIX=\"$(DLL_SUFFIX)\"

EMBED_CFLAGS	= $(CFLAGS) -I$(DEPTH)/lib/plugin
TAINT_CFLAGS	= $(CFLAGS) -I$(DEPTH)/lib/libjar -I$(DEPTH)/sun-java/netscape/security/_jri \
		-I$(DEPTH)/dist/public/security

ifneq ($(OS_ARCH),OS2)
$(OBJDIR)/lm_embed.o: lm_embed.c
	@$(MAKE_OBJDIR)
	$(CC) -o $@ -c $(EMBED_CFLAGS) $<

$(OBJDIR)/lm_taint.o: lm_taint.c
	@$(MAKE_OBJDIR)
	$(CC) -o $@ -c $(TAINT_CFLAGS) $<

else
$(OBJDIR)/lm_embed.o: lm_embed.c
	@$(MAKE_OBJDIR)
	$(CC) -Fo$@ -c $(EMBED_CFLAGS) $<

$(OBJDIR)/lm_taint.o: lm_taint.c
	@$(MAKE_OBJDIR)
	$(CC) -Fo$@ -c $(TAINT_CFLAGS) $<

endif
