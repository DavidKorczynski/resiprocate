
EXTRA_DIST = ca.pem
EXTRA_DIST += *.vcxproj
EXTRA_DIST += testua_readme.txt

LDADD = ../librecon.la
if USE_KURENTO
LDADD += ../../../media/kurento/libkurentoclient.la
endif
if USE_GSTREAMER
LDADD += $(GSTREAMERMM_1_0_LIBS) -lgstwebrtc-1.0
endif
LDADD += ../../../reflow/libreflow.la
LDADD += ../../../media/libresipmedia.la
# These need to be included for static linking, and we need to be able
# to configure the runtime location to find them for dynamic loading
#LDADD += ../../../../sipXtapi/sipXmediaLib/src/mp/codecs/plgl16/codec_l16.la
#LDADD += ../../../../sipXtapi/sipXmediaLib/src/mp/codecs/plgtones/codec_tones.la
#LDADD += ../../../../sipXtapi/sipXmediaLib/src/mp/codecs/plgpcmapcmu/codec_pcmapcmu.la
#LDADD += ../../../../sipXtapi/sipXmediaLib/src/mp/codecs/plgilbc/codec_ilbc.la
LDADD += ../../dum/libdum.la
LDADD += ../../stack/libresip.la
LDADD += ../../../rutil/librutil.la
LDADD += $(LIBSSL_LIBADD) @LIBPTHREAD_LIBADD@

AM_CPPFLAGS = -I$(top_srcdir)/resip/recon

TESTS =
bin_PROGRAMS =
check_PROGRAMS =

if USE_SIPXTAPI

TESTS += sdpTests
TESTS += unitTests
bin_PROGRAMS += testUA
check_PROGRAMS += \
	sdpTests \
	unitTests

testUA_SOURCES = testUA.cxx ../../../media/samples/playback_prompt.h ../../../media/samples/record_prompt.h
sdpTests_SOURCES = sdpTests.cxx
unitTests_SOURCES = unitTests.cxx
testUA_LDADD = -lsipXport
if USE_KURENTO
testUA_LDADD += -lboost_system
endif
testUA_LDADD += $(LDADD)

endif



####################################################################
#
# Copyright (c) 2007-2008, Plantronics, Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are 
# met:
#
# 1. Redistributions of source code must retain the above copyright 
#    notice, this list of conditions and the following disclaimer. 
#
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution. 
#
# 3. Neither the name of Plantronics nor the names of its contributors 
#    may be used to endorse or promote products derived from this 
#    software without specific prior written permission. 
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR 
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT 
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
####################################################################
