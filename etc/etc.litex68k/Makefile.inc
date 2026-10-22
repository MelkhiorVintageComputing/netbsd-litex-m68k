#	$NetBSD$
#
#	etc.litex68k/Makefile.inc -- litex68k-specific etc Makefile targets
#

# If you change the list of distributed kernels, don't forget
# to update the release documentation in distrib/notes/common/contents

KERNEL_SETS=		GENERIC

INSTALLATION_DIRS+=	installation/ramdisks

snap_md_post:
	${MAKESUMS} -t ${RELEASEDIR}/${RELEASEMACHINEDIR}/installation/ramdisks -a
