

AUTOMAKE_OPTIONS = foreign dist-zip
ACLOCAL_AMFLAGS = -I m4

SUBDIRS = rutil
SUBDIRS += resip
if BUILD_REPRO
SUBDIRS += repro
endif
if BUILD_TFM
SUBDIRS += tfm
endif

# The Makefiles for these don't separate SSL stuff yet...
if USE_SSL
SUBDIRS += media
if BUILD_RETURN
SUBDIRS += reTurn
if BUILD_RECON
SUBDIRS += reflow
SUBDIRS += resip/recon
endif
endif
if BUILD_P2P
SUBDIRS += p2p/s2c/s2c p2p
endif
endif

if BUILD_APPS
SUBDIRS += apps
endif

#EXTRA_DIST = build-contrib.sh

EXTRA_DIST = *.sln

EXTRA_DIST += resiprocate.spec

EXTRA_DIST += CCACHE_and_DISTCC.txt
EXTRA_DIST += RELEASE-PROCESS.txt
EXTRA_DIST += README_AMQP_Apache_Qpid_Proton.txt
EXTRA_DIST += README_Android.txt
EXTRA_DIST += README_RPM-based-Linux.txt
EXTRA_DIST += build/configure-android.sh
EXTRA_DIST += build/debian.sh
EXTRA_DIST += build/distcc-setup
EXTRA_DIST += build/fedora.sh

EXTRA_DIST += tools/monitor-amqp-queue-topic.py
EXTRA_DIST += tools/send-cmd.py
