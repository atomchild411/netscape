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
#

DEPTH		= ../..

# the tree's libpng: only with NS_TREE_IMGLIBS (NS_SYSTEM_IMGLIBS)
ifdef NS_TREE_IMGLIBS
DIRS_PNG = png
endif

ifdef STANDALONE_IMAGE_LIB
DIRS = $(DIRS_PNG) public src
else
DIRS = $(DIRS_PNG) classes public src
endif

EXTRA_LIBS	= libzlib.a libpng.a

include $(DEPTH)/config/rules.mk
